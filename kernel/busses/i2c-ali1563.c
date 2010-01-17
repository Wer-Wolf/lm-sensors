/**
 *	i2c-ali1563.c - i2c driver for the ALi 1563 Southbridge
 *
 *	Copyright (C) 2004, 2005 Patrick Mochel, Chunhao Huang
 *
 *	The 1563 southbridge is deceptively similar to the 1533, with a
 *	few notable exceptions. One of those happens to be the fact they
 *	upgraded the i2c core to be 2.0 compliant, and happens to be almost
 *	identical to the i2c controller found in the Intel 801 south
 *	bridges.
 *
 *	This driver is based on a mix of the 15x3, 1535, and i801 drivers,
 *	with a little help from the ALi 1563 spec.
 *
 *	Chunhao Huang / Winbond
 *	Backport to linux-2.4 from linux-2.6 on Mar.2005
 *
 *	This file is released under the GPLv2
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/pci.h>
#include <linux/init.h>
#include "version.h"
#include "sensors_compat.h"

#ifndef PCI_DEVICE_ID_AL_M1563
#define PCI_DEVICE_ID_AL_M1563      0x1563
#endif

#define ALI1563_MAX_TIMEOUT	500
#define	ALI1563_SMBBA		0x80
#define ALI1563_SMB_IOEN	1
#define ALI1563_SMB_HOSTEN	2
#define ALI1563_SMB_IOSIZE	16

#define SMB_HST_STS	(ali1563_smba + 0)
#define SMB_HST_CNTL1	(ali1563_smba + 1)
#define SMB_HST_CNTL2	(ali1563_smba + 2)
#define SMB_HST_CMD	(ali1563_smba + 3)
#define SMB_HST_ADD	(ali1563_smba + 4)
#define SMB_HST_DAT0	(ali1563_smba + 5)
#define SMB_HST_DAT1	(ali1563_smba + 6)
#define SMB_BLK_DAT	(ali1563_smba + 7)

#define HST_STS_BUSY	0x01
#define HST_STS_INTR	0x02
#define HST_STS_DEVERR	0x04
#define HST_STS_BUSERR	0x08
#define HST_STS_FAIL	0x10
#define HST_STS_DONE	0x80
#define HST_STS_BAD	0x1c


#define HST_CNTL1_TIMEOUT	0x80
#define HST_CNTL1_LAST		0x40

#define HST_CNTL2_KILL		0x04
#define HST_CNTL2_START		0x40
#define HST_CNTL2_QUICK		0x00
#define HST_CNTL2_BYTE		0x01
#define HST_CNTL2_BYTE_DATA	0x02
#define HST_CNTL2_WORD_DATA	0x03
#define HST_CNTL2_BLOCK		0x05
#define HST_CNTL2_SIZEMASK	0x38

static struct pci_driver ali1563_pci_driver;
static unsigned short ali1563_smba;

static int ali1563_transaction(struct i2c_adapter * a)
{
	u32 data;
	int timeout;

	printk(KERN_DEBUG "ali1563: Transaction (pre): STS=%02x, CNTL1=%02x, "
		"CNTL2=%02x, CMD=%02x, ADD=%02x, DAT0=%02x, DAT1=%02x\n",
		inb_p(SMB_HST_STS), inb_p(SMB_HST_CNTL1), inb_p(SMB_HST_CNTL2),
		inb_p(SMB_HST_CMD), inb_p(SMB_HST_ADD), inb_p(SMB_HST_DAT0),
		inb_p(SMB_HST_DAT1));

	data = inb_p(SMB_HST_STS);
	if (data & HST_STS_BAD) {
		printk(KERN_WARNING "ali1563: Trying to reset busy device\n");
		outb_p(data | HST_STS_BAD,SMB_HST_STS);
		data = inb_p(SMB_HST_STS);
		if (data & HST_STS_BAD){
			return -EBUSY;
		}
	}
	outb_p(inb_p(SMB_HST_CNTL2) | HST_CNTL2_START, SMB_HST_CNTL2);

	timeout = ALI1563_MAX_TIMEOUT;
	do {
		i2c_delay(1);
	} while (((data = inb_p(SMB_HST_STS)) & HST_STS_BUSY) && --timeout);

	printk(KERN_DEBUG "ali1563: Transaction (post): STS=%02x, CNTL1=%02x, "
		"CNTL2=%02x, CMD=%02x, ADD=%02x, DAT0=%02x, DAT1=%02x\n",
		inb_p(SMB_HST_STS), inb_p(SMB_HST_CNTL1), inb_p(SMB_HST_CNTL2),
		inb_p(SMB_HST_CMD), inb_p(SMB_HST_ADD), inb_p(SMB_HST_DAT0),
		inb_p(SMB_HST_DAT1));

	if (timeout && !(data & HST_STS_BAD)) {
		return 0;
	}

	/* modified by Rudolf */
	if (!timeout) {
		printk(KERN_ERR "ali1563: Timeout - Trying to KILL transaction!\n");
		/* Issue 'kill' to host controller */
		outb_p(HST_CNTL2_KILL,SMB_HST_CNTL2);
		data = inb_p(SMB_HST_STS);		
 	}
	
	/* device error - probably missing ACK */	
	if (data & HST_STS_DEVERR) {
		printk(KERN_DEBUG "ali1563: Device error!\n");
	}

	/* bus collision */
	if (data & HST_STS_BUSERR) {
		printk(KERN_ERR "ali1563: Bus collision!\n");
		/* Issue timeout, hoping it helps */
		outb_p(HST_CNTL1_TIMEOUT,SMB_HST_CNTL1);
	}

	if (data & HST_STS_FAIL) {
		printk(KERN_ERR "ali1563: Cleaning fail after KILL!\n");
		outb_p(0x0,SMB_HST_CNTL2);
	}

	return -1;
}

static int ali1563_block_start(struct i2c_adapter * a)
{
	u32 data;
	int timeout;

	printk(KERN_DEBUG "ali1563: Block (pre): STS=%02x, CNTL1=%02x, "
		"CNTL2=%02x, CMD=%02x, ADD=%02x, DAT0=%02x, DAT1=%02x\n",
		inb_p(SMB_HST_STS), inb_p(SMB_HST_CNTL1), inb_p(SMB_HST_CNTL2),
		inb_p(SMB_HST_CMD), inb_p(SMB_HST_ADD), inb_p(SMB_HST_DAT0),
		inb_p(SMB_HST_DAT1));

	data = inb_p(SMB_HST_STS);
	if (data & HST_STS_BAD) {
		printk(KERN_WARNING "ali1563: Trying to reset busy device\n");
		outb_p(data | HST_STS_BAD,SMB_HST_STS);
		data = inb_p(SMB_HST_STS);
		if (data & HST_STS_BAD){
			return -EBUSY;
		}
	}

	/* Clear byte-ready bit */
	outb_p(data | HST_STS_DONE, SMB_HST_STS);

	/* Start transaction and wait for byte-ready bit to be set */
	outb_p(inb_p(SMB_HST_CNTL2) | HST_CNTL2_START, SMB_HST_CNTL2);

	timeout = ALI1563_MAX_TIMEOUT;
	do {
		i2c_delay(1);
	} while (!((data = inb_p(SMB_HST_STS)) & HST_STS_DONE) && --timeout);

	printk(KERN_DEBUG "ali1563: Block (post): STS=%02x, CNTL1=%02x, "
		"CNTL2=%02x, CMD=%02x, ADD=%02x, DAT0=%02x, DAT1=%02x\n",
		inb_p(SMB_HST_STS), inb_p(SMB_HST_CNTL1), inb_p(SMB_HST_CNTL2),
		inb_p(SMB_HST_CMD), inb_p(SMB_HST_ADD), inb_p(SMB_HST_DAT0),
		inb_p(SMB_HST_DAT1));

	if (timeout && !(data & HST_STS_BAD)){
		return 0;
	}
	printk(KERN_WARNING "ali1563: SMBus Error: %s%s%s%s%s\n",
		timeout ? "Timeout " : "",
		data & HST_STS_FAIL ? "Transaction Failed " : "",
		data & HST_STS_BUSERR ? "No response or Bus Collision " : "",
		data & HST_STS_DEVERR ? "Device Error " : "",
		!(data & HST_STS_DONE) ? "Transaction Never Finished " : "");
	return -1;
}

static int ali1563_block(struct i2c_adapter * a, union i2c_smbus_data * data, u8 rw)
{
	int i, len;
	int error = 0;

	/* Do we need this? */
	outb_p(HST_CNTL1_LAST,SMB_HST_CNTL1);

	if (rw == I2C_SMBUS_WRITE) {
		len = data->block[0];
		if (len < 1)
			len = 1;
		else if (len > 32)
			len = 32;
		outb_p(len,SMB_HST_DAT0);
		outb_p(data->block[1],SMB_BLK_DAT);
	} else
		len = 32;

	outb_p(inb_p(SMB_HST_CNTL2) | HST_CNTL2_BLOCK, SMB_HST_CNTL2);

	for (i = 0; i < len; i++) {
		if (rw == I2C_SMBUS_WRITE) {
			outb_p(data->block[i + 1], SMB_BLK_DAT);
			if ((error = ali1563_block_start(a)))
				break;
		} else {
			if ((error = ali1563_block_start(a)))
				break;
			if (i == 0) {
				len = inb_p(SMB_HST_DAT0);
				if (len < 1)
					len = 1;
				else if (len > 32)
					len = 32;
			}
			data->block[i+1] = inb_p(SMB_BLK_DAT);
		}
	}
	/* Do we need this? */
	outb_p(HST_CNTL1_LAST,SMB_HST_CNTL1);
	return error;
}

static s32 ali1563_access(struct i2c_adapter * a, u16 addr,
			  unsigned short flags, char rw, u8 cmd,
			  int size, union i2c_smbus_data * data)
{
	int error = 0;
	int timeout;
	u32 reg;

	for (timeout = ALI1563_MAX_TIMEOUT; timeout; timeout--) {
		if (!(reg = inb_p(SMB_HST_STS) & HST_STS_BUSY))
			break;
	}
	if (!timeout)
		printk(KERN_WARNING "ali1563: SMBus not idle. HST_STS = %02x\n",reg);
	outb_p(0xff,SMB_HST_STS);

	/* Map the size to what the chip understands */
	switch (size) {
	case I2C_SMBUS_PROC_CALL:
		printk(KERN_ERR "ali1563: I2C_SMBUS_PROC_CALL not supported!\n");
		error = -EINVAL;
		break;
	case I2C_SMBUS_QUICK:
		size = HST_CNTL2_QUICK;
		break;
	case I2C_SMBUS_BYTE:
		size = HST_CNTL2_BYTE;
		break;
	case I2C_SMBUS_BYTE_DATA:
		size = HST_CNTL2_BYTE_DATA;
		break;
	case I2C_SMBUS_WORD_DATA:
		size = HST_CNTL2_WORD_DATA;
		break;
	case I2C_SMBUS_BLOCK_DATA:
		size = HST_CNTL2_BLOCK;
		break;
	}

	outb_p(((addr & 0x7f) << 1) | (rw & 0x01), SMB_HST_ADD);
	outb_p( (inb_p(SMB_HST_CNTL2)&~HST_CNTL2_SIZEMASK) | (size << 3), SMB_HST_CNTL2);

	/* Write the command register */
	switch(size) {
	case HST_CNTL2_BYTE:
		if (rw== I2C_SMBUS_WRITE)
			/* outb_p(cmd, SMB_HST_CMD); */
			outb_p(cmd, SMB_HST_DAT0); /* modify by Rudolf */
		break;
	case HST_CNTL2_BYTE_DATA:
		outb_p(cmd, SMB_HST_CMD);
		if (rw == I2C_SMBUS_WRITE)
			outb_p(data->byte, SMB_HST_DAT0);
		break;
	case HST_CNTL2_WORD_DATA:
		outb_p(cmd, SMB_HST_CMD);
		if (rw == I2C_SMBUS_WRITE) {
			outb_p(data->word & 0xff, SMB_HST_DAT0);
			outb_p((data->word & 0xff00) >> 8, SMB_HST_DAT1);
		}
		break;
	case HST_CNTL2_BLOCK:
		outb_p(cmd, SMB_HST_CMD);
		error = ali1563_block(a,data,rw);
		goto Done;
	}

	if ((error = ali1563_transaction(a))){
		goto Done;
	}

	if ((rw == I2C_SMBUS_WRITE) || (size == HST_CNTL2_QUICK)){
		goto Done;
	}

	switch (size) {
	case HST_CNTL2_BYTE:	/* Result put in SMBHSTDAT0 */
		data->byte = inb_p(SMB_HST_DAT0);
		break;
	case HST_CNTL2_BYTE_DATA:
		data->byte = inb_p(SMB_HST_DAT0);
		break;
	case HST_CNTL2_WORD_DATA:
		data->word = inb_p(SMB_HST_DAT0) + (inb_p(SMB_HST_DAT1) << 8);
		break;
	}
Done:
	return error;
}

static u32 ali1563_func(struct i2c_adapter * a)
{
	return ( I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |
	    I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
	    I2C_FUNC_SMBUS_BLOCK_DATA );
}


static int __devinit ali1563_setup(struct pci_dev * dev)
{
	u16 ctrl;

	pci_read_config_word(dev,ALI1563_SMBBA,&ctrl);

	/* SMB I/O Base in high 12 bits and must be aligned with the
	 * size of the I/O space. */
	ali1563_smba = ctrl & ~(ALI1563_SMB_IOSIZE - 1);
	if (!ali1563_smba) {
		printk(KERN_WARNING "ali1563: ali1563_smba Uninitialized\n");
		goto Err;
	}

	/* Check if device is enabled */
	if (!(ctrl & ALI1563_SMB_HOSTEN)) {
		printk(KERN_WARNING "ali1563: Host Controller not enabled\n");
		goto Err;
	}
	if (!(ctrl & ALI1563_SMB_IOEN)) {
		printk(KERN_WARNING "I/O space not enabled, trying manually\n");
		pci_write_config_word(dev, ALI1563_SMBBA,
				      ctrl | ALI1563_SMB_IOEN);
		pci_read_config_word(dev, ALI1563_SMBBA, &ctrl);
		if (!(ctrl & ALI1563_SMB_IOEN)) {
			printk(KERN_WARNING "I/O space still not enabled, "
			       "giving up\n");
			goto Err;
		}
	}

	if (!request_region(ali1563_smba, ALI1563_SMB_IOSIZE,
			    ali1563_pci_driver.name)) {
		printk(KERN_WARNING "ali1563: Could not allocate I/O space "
		       "at 0x%04x\n", ali1563_smba);
		goto Err;
	}
	printk(KERN_INFO "ali1563: Found ALi1563 SMBus at 0x%04x\n",
	       ali1563_smba);

	return 0;
Err:
	return -ENODEV;
}

static void ali1563_shutdown(struct pci_dev *dev)
{
	release_region(ali1563_smba,ALI1563_SMB_IOSIZE);
}

static void ali1563_inc(struct i2c_adapter *adapter)
{
#ifdef MODULE
	MOD_INC_USE_COUNT;
#endif
}

static void ali1563_dec(struct i2c_adapter *adapter)
{
#ifdef MODULE
	MOD_DEC_USE_COUNT;
#endif
}

static struct i2c_algorithm ali1563_algorithm = {
	.name		= "Non-i2c SMBus adapter",
	.id		= I2C_ALGO_SMBUS,
	.smbus_xfer	= ali1563_access,
	.functionality	= ali1563_func,
};

static struct i2c_adapter ali1563_adapter = {
	.algo		= &ali1563_algorithm,
	.inc_use	= ali1563_inc,
	.dec_use	= ali1563_dec,
};

static int __devinit ali1563_probe(struct pci_dev * dev,
				const struct pci_device_id * id_table)
{
	int error;

	if ((error = ali1563_setup(dev))) {
		printk(KERN_WARNING "ali1563: ALi1563 SMBus probe failed "
		       "(%d)\n", error);
		return error;
	}
	sprintf(ali1563_adapter.name,"SMBus ALi 1563 Adapter @ %04x",
		ali1563_smba);
	if ((error = i2c_add_adapter(&ali1563_adapter)))
		ali1563_shutdown(dev);
	return error;
}

static void __devexit ali1563_remove(struct pci_dev * dev)
{
	i2c_del_adapter(&ali1563_adapter);
	ali1563_shutdown(dev);
}

static struct pci_device_id ali1563_ids[] __devinitdata = {
	{
	.vendor =       PCI_VENDOR_ID_AL,
	.device =       PCI_DEVICE_ID_AL_M1563,
	.subvendor =    PCI_ANY_ID,
	.subdevice =    PCI_ANY_ID,
	},
	{ 0, }
};

static struct pci_driver ali1563_pci_driver = {
 	.name		= "ali1563 smbus",
	.id_table	= ali1563_ids,
 	.probe		= ali1563_probe,
	.remove		= __devexit_p(ali1563_remove),
};

static int __init ali1563_init(void)
{
	printk("i2c-ali1563.o version %s (%s)\n", LM_VERSION, LM_DATE);
	return pci_module_init(&ali1563_pci_driver);
}

module_init(ali1563_init);

static void __exit ali1563_exit(void)
{
	pci_unregister_driver(&ali1563_pci_driver);
}

module_exit(ali1563_exit);

MODULE_AUTHOR("Patrick Mochel <mochel@digitalimplant.org>, Chunhao Huang");
MODULE_DESCRIPTION("ALi M1563 southbridge driver for linux-2.4, backported by Chunhao Huang from linux-2.6");
MODULE_LICENSE("GPL");


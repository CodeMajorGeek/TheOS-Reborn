#include <Device/ATA.h>

#include <stdio.h>

void ATA_init(void)
{
    IO_outb(ATA_PRIMARY_CTRL_BUS, 0);
    IO_outb(ATA_SECONDARY_CTRL_BUS, 0);

    ATA_Device_t dev = { ATA_PRIMARY_IO_BUS, ATA_PRIMARY_CTRL_BUS };
    int devtype = ATA_detect_devtype(128, &dev);

    printf("The ATA device type is: %d\n", devtype);
}

void ATA_software_reset(uint16_t bus)
{
    IO_outb(bus, ATA_DEV_SRST);
    IO_wait();
    IO_outb(bus, 0);
    IO_wait();
}

int ATA_detect_devtype(int slavebit, ATA_Device_t* ctrl)
{
    ATA_software_reset(ATA_PRIMARY_CTRL_BUS);
    IO_outb(ctrl->base + ATA_REG_DEVSEL, 0xA0 | slavebit << 4);

    for (uint8_t i = 0; i < 4; i++)
        IO_inb(ctrl->dev_ctrl);

    unsigned cl = IO_inb(ctrl->base + ATA_REG_CYL_LO);
    unsigned ch = IO_inb(ctrl->base + ATA_REG_CYL_HI);

    if (cl == 0x14 && ch == 0xEB)
        return ATADEV_PATAPI;
    else if (cl == 0x69 && ch == 0x96)
        return ATADEV_SATAPI;
    else if (cl == 0 && ch == 0)
        return ATADEV_PATA;
    else if (cl == 0x3C && ch == 0x3C)
        return ATADEV_SATA;
    else
        return ATADEV_UNKNOWN;
}
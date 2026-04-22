#include <stdio.h>
#include "altera_up_avalon_accelerometer_spi.h"

int main(void) {
    alt_up_accelerometer_spi_dev *accel =
        alt_up_accelerometer_spi_open_dev("/dev/accelerometer_spi_0");

    if (!accel) {
        printf("Failed to open accelerometer\n");
        return 1;
    }

    alt_32 x, y, z;
    while (1) {
        alt_up_accelerometer_spi_read_x_axis(accel, &x);
        alt_up_accelerometer_spi_read_y_axis(accel, &y);
        alt_up_accelerometer_spi_read_z_axis(accel, &z);
        printf("X=%ld, Y=%ld, Z=%ld\n", x, y, z);

        usleep(1000000);
    }
    return 0;
}


#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define LENGTH 320
#define WIDTH 240
#define RADIAN 3.142/180

float X, Y, Z;
float scale_const = 40;
float focal = 100;
float cube_size = 40;

uint8_t buffer_2d[LENGTH * WIDTH];
float buffer_z[LENGTH * WIDTH];

float normal_x = 0;
float normal_y = 0;
float normal_z = 0;
float light_x = 0;
float light_y = 0;
float light_z = -1;

float x_pos(float x, float y, float z) {
    return x * (cos(X) * cos(Y)) +
           y * (cos(X) * sin(Y) * sin(Z) - sin(X) * cos(Z)) +
           z * (cos(X) * sin(Y) * cos(Z) + sin(X) * sin(Z));
}

float y_pos(float x, float y, float z) {
    return x * (sin(X) * cos(Y)) +
           y * (sin(X) * sin(Y) * sin(Z) + cos(X) * cos(Z)) +
           z * (sin(X) * sin(Y) * cos(Z) - cos(X) * sin(Z));
}

float z_pos(float x, float y, float z) {
    return x * (-sin(Y)) +
           y * (cos(Y) * sin(Z)) +
           z * (cos(Y) * cos(Z));
}

void translate_point(float cube_x, float cube_y, float cube_z, float normal_x, float normal_y, float normal_z) {
    float x = x_pos(cube_x, cube_y, cube_z);
    float y = y_pos(cube_x, cube_y, cube_z);
    float z = z_pos(cube_x, cube_y, cube_z) + focal;

    float nx = x_pos(normal_x, normal_y, normal_z);
    float ny = y_pos(normal_x, normal_y, normal_z);
    float nz = z_pos(normal_x, normal_y, normal_z);

    float ooz = 1.0f/z;

    float brightness = nx * light_x + ny * light_y + nz * light_z;
    if (brightness < 0) {
        brightness = 0;
    } else if (brightness > 1) {
        brightness = 1;
    }

    int shade = (int)(brightness * 15);

    int xp = (int)(LENGTH/2 + scale_const * x * ooz * 2);
    int yp = (int)(WIDTH/2 + scale_const * y * ooz);

    if (ooz > buffer_z[xp + LENGTH * yp]) {
        buffer_2d[xp + LENGTH * yp] = shade;
        buffer_z[xp + LENGTH * yp] = ooz;
    }
}

// take in accelerometer value to render the cube
void render_cube(alt_32 x, alt_32 y, alt_32 z) {
	// TODO use range of accelerometer value to get x, y and z
	for (int i = 0; i < LENGTH * WIDTH; i++) {
		buffer_2d[i] = 0;
		buffer_z[i] = 0;
	}
    for (float cube_x = -cube_size; cube_x < cube_size; cube_x++) {
        for (float cube_y = -cube_size; cube_y < cube_size; cube_y++) {
            for (float cube_z = -cube_size; cube_z < cube_size; cube_z++) {
                normal_x = 0;
                normal_y = 0;
                normal_z = 0;

                if (cube_x == cube_size - 1) {
                    normal_x = 1;
                } else if (cube_x == -cube_size) {
                    normal_x = -1;
                }

                if (cube_y == cube_size - 1) {
                    normal_y = 1;
                } else if (cube_y == -cube_size) {
                    normal_y = -1;
                }

                if (cube_z == cube_size - 1) {
                    normal_z = 1;
                } else if (cube_z == -cube_size) {
                    normal_z = -1;
                }

                float mag = sqrt(normal_x * normal_x + normal_y * normal_y + normal_z * normal_z);
                if (mag > 0) {
                    normal_x /= mag;
                    normal_y /= mag;
                    normal_z /= mag;
                }

                if (cube_x == -cube_size || cube_x == cube_size - 1 || cube_y == -cube_size || cube_y == cube_size - 1 || cube_z == -cube_size || cube_z == cube_size - 1) {
                    translate_point(cube_x, cube_y, cube_z, normal_x, normal_y, normal_z);
                }
            }
        }
    }
    // TODO write buffer_2d into address that is meant to be used in write_pixel
}

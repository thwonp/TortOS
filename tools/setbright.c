/* SPDX-License-Identifier: MIT
 *
 * Set the LCD backlight on the TrimUI Brick (tg3040) straight through the
 * display-engine ioctl, so the boot animation is at the configured brightness
 * from its first frame rather than the hardware default. launch.sh runs this
 * before anything is drawn, because the launcher that would normally apply
 * the setting has not started yet -- and the step in brightness partway
 * through a boot is exactly the kind of seam this firmware is trying not to
 * have. Usage: setbright <raw 0-255>
 */
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DISP_LCD_SET_BRIGHTNESS 0x102

int main(int argc, char *argv[])
{
	if (argc < 2) return 2;
	int val = atoi(argv[1]);
	if (val < 0) val = 0;
	if (val > 255) val = 255;
	int fd = open("/dev/disp", O_RDWR);
	if (fd < 0) return 1;
	unsigned long param[4] = { 0, (unsigned long)val, 0, 0 };
	ioctl(fd, DISP_LCD_SET_BRIGHTNESS, param);
	close(fd);
	return 0;
}

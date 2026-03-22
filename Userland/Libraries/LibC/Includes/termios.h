#ifndef _TERMIOS_H
#define _TERMIOS_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#ifndef NCCS
#define NCCS 32
#endif

struct termios
{
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

/* c_cc character slots */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSTART   8
#define VSTOP    9
#define VSUSP    10

/* c_iflag */
#define IGNBRK   0000001U
#define BRKINT   0000002U
#define IGNPAR   0000004U
#define PARMRK   0000010U
#define INPCK    0000020U
#define ISTRIP   0000040U
#define INLCR    0000100U
#define IGNCR    0000200U
#define ICRNL    0000400U
#define IXON     0002000U
#define IXOFF    0010000U

/* c_oflag */
#define OPOST    0000001U
#define ONLCR    0000004U

/* c_cflag */
#define CSIZE    0000060U
#define CS8      0000060U
#define CREAD    0000200U
#define PARENB   0000400U

/* c_lflag */
#define ISIG     0000001U
#define ICANON   0000002U
#define ECHO     0000010U
#define ECHOE    0000020U
#define ECHOK    0000040U
#define ECHONL   0000100U
#define NOFLSH   0000200U
#define TOSTOP   0000400U
#define IEXTEN   0100000U

/* tcsetattr optional_actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* tcflush queue_selector */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

/* Speed values (POSIX/GNU common subset). */
#define B0       0U
#define B50      50U
#define B75      75U
#define B110     110U
#define B134     134U
#define B150     150U
#define B200     200U
#define B300     300U
#define B600     600U
#define B1200    1200U
#define B1800    1800U
#define B2400    2400U
#define B4800    4800U
#define B9600    9600U
#define B19200   19200U
#define B38400   38400U

int tcgetattr(int fd, struct termios* termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios* termios_p);
int tcdrain(int fd);
int tcflush(int fd, int queue_selector);
int tcsendbreak(int fd, int duration);

speed_t cfgetispeed(const struct termios* termios_p);
speed_t cfgetospeed(const struct termios* termios_p);
int cfsetispeed(struct termios* termios_p, speed_t speed);
int cfsetospeed(struct termios* termios_p, speed_t speed);
void cfmakeraw(struct termios* termios_p);

#ifdef __cplusplus
}
#endif

#endif

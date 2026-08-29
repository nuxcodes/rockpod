#ifndef SERIAL_H
#define SERIAL_H
void serial_setup(void);
void serial_bitrate(int rate);
int  tx_rdy(void);
void tx_writec(unsigned char c);
#endif

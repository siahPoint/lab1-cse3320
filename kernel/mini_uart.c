#define K2_DEBUG_WARN 

#include <stdint.h>
#include "plat.h"
#include "utils.h"

/* ---------------- gpio ------------------------------------ 
    cf BCM2837 manual, chap 6, "General Purpose I/O (GPIO)"
*/
#define GPFSEL1         (PBASE+0x00200004)    // "GPIO Function Select"
#define GPSET0          (PBASE+0x0020001C)    // "GPIO Pin Output Set"
#define GPCLR0          (PBASE+0x00200028)    // "GPIO Pin Output Clear"
#define GPPUD           (PBASE+0x00200094)    // "GPIO Pin Pull-up/down Enable"
#define GPPUDCLK0       (PBASE+0x00200098)    // "GPIO Pin Pull-up/down Enable Clock"

/* 
 * ---------------- mini uart ------------------------------------ 
 * "The Device has three Auxiliary peripherals: One mini UART and two SPI masters. These
 * three peripherals are grouped together as they share the same area in the peripheral register
 * map and they share a common interrupt."
 */
#define AUXIRQ          (PBASE+0x00215000)    // bit0: "If set the mini UART has an interrupt pending"
#define AUX_ENABLES     (PBASE+0x00215004)    // "AUXENB" in datasheet
#define AUX_MU_IO_REG   (PBASE+0x00215040)
#define AUX_MU_IER_REG  (PBASE+0x00215044)    // enable tx/rx irqs
  #define AUX_MU_IER_RXIRQ_ENABLE 1U      
  #define AUX_MU_IER_TXIRQ_ENABLE 2U      

#define AUX_MU_IIR_REG  (PBASE+0x00215048)    // check irq cause, fifo clear
  #define IS_TRANSMIT_INTERRUPT(x) (x & 0x2)  //0b010  tx empty
  #define IS_RECEIVE_INTERRUPT(x) (x & 0x4)   //0b100  rx ready
  #define FLUSH_UART 0xC6

#define AUX_MU_LCR_REG  (PBASE+0x0021504C)
#define AUX_MU_MCR_REG  (PBASE+0x00215050)

#define AUX_MU_LSR_REG  (PBASE+0x00215054)
  #define IS_TRANSMITTER_EMPTY(x) (x & 0x10)
  #define IS_TRANSMITTER_IDLE(x)  (x & 0x20)
  #define IS_DATA_READY(x) (x & 0x1)
  #define IS_RECEIVER_OVEERUN(x) (x & 0x2)
#define AUX_MU_MSR_REG  (PBASE+0x00215058)

#define AUX_MU_SCRATCH  (PBASE+0x0021505C)
#define AUX_MU_CNTL_REG (PBASE+0x00215060)
#define AUX_MU_STAT_REG (PBASE+0x00215064) // rx fifo stat
#define AUX_MU_BAUD_REG (PBASE+0x00215068)

// busy wait. cf uart_recv() for the receive-side counterpart
void uart_send (char c) {
	while(1) {
        // read the status reg to check if the tx fifo is empty
		if(get32(AUX_MU_LSR_REG) & 0x20)
			break;
	}
	put32(AUX_MU_IO_REG, c);    // write the char to the tx fifo
}
 
// busy wait until get a char 
char uart_recv (void) {
	while(1) {
		if(get32(AUX_MU_LSR_REG) & 0x01) 
			break;
	}
	return(get32(AUX_MU_IO_REG) & 0xFF);
}

// try read a char, return -1 if no char (NB: return type is int) 
int uart_try_recv(void) {
    if (!(get32(AUX_MU_STAT_REG) & 0xF0000)) {
        return -1;
    } else {
        // rx fifo has bytes
        return get32(AUX_MU_IO_REG) & 0xFF;
    }
}

void uart_send_string(char* str) {
	for (int i = 0; str[i] != '\0'; i ++) {
		uart_send((char)str[i]);
	}
}

// This function is required by printf function
void putc(void* p, char c) {
	uart_send(c);
}

void test_ktimer2(int c); // unittest.c

/*
 * Handle a UART interrupt, raised because input has
 * arrived. Only handle RX IRQ for simplicity.
 * Called from handle_irq().
 */
void uart_irq(void) {
    //  check AUX_MU_IIR_REG bit0 for pending irq
    //    and bit 2:1 for irq causes
	int c; 
    uint iir = get32(AUX_MU_IIR_REG);
    if (iir & 1) // no pending
        return;
    V("pending irq: p %d w %d r %d", (iir & 1), (iir & 2), (iir & 4));

    // clear rx irq, must be done before we read 
    if (IS_RECEIVE_INTERRUPT(iir)) {
        while (1) {
            // read a char, if there's no more, break
            // quest: UART rx irq
            c = uart_try_recv();
            if (c == -1)
                break;
			V("char %d", c); 
            test_ktimer2(c);
        }
    }
}

void uart_init(void) {
    unsigned int selector;
    // code below also showcases how to configure GPIO pins
    // cf: https://github.com/bztsrc/raspi3-tutorial/blob/master/03_uart1/uart.c#L45
    // select gpio functions for pin14,15. note 3bits per pin.
    selector = get32(GPFSEL1);
    selector &= ~(7 << 12); // clean gpio14 (12 is not a typo)
    selector |= 2 << 12;    // set alt5 for gpio14
    selector &= ~(7 << 15); // clean gpio15
    selector |= 2 << 15;    // set alt5 for gpio15
    put32(GPFSEL1, selector);

    // Below: set up GPIO pull modes. protocol recommended by the bcm2837 manual
    //    (pg 101, "GPIO Pull-up/down Clock Registers")
    // We need neither the pull-up nor the pull-down state, because both
    //  the 14 and 15 pins are going to be connected all the time.
    put32(GPPUD, 0); // disable pull up/down control (for pins below)
    delay(150);
    // "control the actuation of internal pull-downs on the respective GPIO pins."
    put32(GPPUDCLK0, (1 << 14) | (1 << 15)); // "clock the control signal into the GPIO pads"
    delay(150);
    put32(GPPUDCLK0, 0);               // remote the clock, flush GPIO setup
    put32(AUX_MU_IIR_REG, FLUSH_UART); // flush FIFO

    put32(AUX_ENABLES, 1);     // Enable mini uart (this also enables access to it registers)
    put32(AUX_MU_CNTL_REG, 0); // Disable auto flow control and disable receiver and transmitter (for now)

	// Disable receive and transmit interrupts
    put32(AUX_MU_IER_REG, (3 << 2) | (0xf << 4)); // bit 7:4 3:2 must be 1
    // quest: UART rx irq
	{ // enable rx irq
		unsigned int ier = get32(AUX_MU_IER_REG); 
        // flip the bits of ier that enable rx irq, and write back ier to the reg
        put32(AUX_MU_IER_REG, ier | AUX_MU_IER_RXIRQ_ENABLE);
	} // leave tx irq disabled

    put32(AUX_MU_LCR_REG, 3);    // Enable 8 bit mode
    put32(AUX_MU_MCR_REG, 0);    // Set RTS line to be always high
    put32(AUX_MU_BAUD_REG, 270); // Set baud rate to 115200

    put32(AUX_MU_CNTL_REG, 3); // Finally, enable transmitter and receiver
}

// cf: https://github.com/bztsrc/raspi3-tutorial/blob/master/03_uart1/uart.c
// cf: https://github.com/futurehomeno/RPI_mini_UART/tree/master

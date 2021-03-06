#ifndef AD5206_H
#define AD5206_H

#include "libs/Kernel.h"
#include "libs/utils.h"
#include <libs/Pin.h>
#include "mbed.h"
#include <string>
#include <math.h>

#define max(a,b) (((a) > (b)) ? (a) : (b))

class AD5206 : public DigipotBase {
    public:
        AD5206(){
            this->spi= new mbed::SPI(P0_9,P0_8,P0_7); //should be able to set those pins in config
            cs.from_string("4.29")->as_output(); //this also should be configurable
            cs.set(1);
        }

        void set_current( int channel, double current )
        {
            current = min( max( current, 0.0L ), 2.0L );


            char adresses[6] = { 0x05, 0x03, 0x01, 0x00, 0x02, 0x04 };
            cs.set(0);
            spi->write((int)adresses[channel]);
            spi->write((int)current_to_wiper(current));
            cs.set(1);
        }


        //taken from 4pi firmware
        unsigned char current_to_wiper( double current ){
            unsigned int count = int((current*1000)*100/743); //6.8k resistor and 10k pot

            return (unsigned char)count;
        }

        double get_current(int channel)
        {
            return currents[channel];
        }

    private:

        Pin cs;
        mbed::SPI* spi;
        double currents[6];
};


#endif

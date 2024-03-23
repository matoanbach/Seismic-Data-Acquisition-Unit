// DataAcquisitionMain.cpp - Receive seismic activity from a transducer

// Ma Toan Bach

#include "DataAcquisition.h"

int main()
{
    int retVal = 0;
    DataAc data;
    retVal = data.run();
    return retVal;
}

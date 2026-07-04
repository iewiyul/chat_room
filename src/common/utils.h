#pragma once
#include <fcntl.h>
#include <unistd.h>

namespace chat::common{
    inline void setNonBlocking(int fd){
        int flags=::fcntl(fd,F_GETFL,0);
        if(flags>=0){
            ::fcntl(fd,F_SETFL,flags | O_NONBLOCK);
        }
    }
}
#include "ws_connection.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace chat::server{

    WsConnection::WsConnection(int fd, Reactor& reactor, std::string username)
        : fd_(fd), reactor_(reactor), username_(std::move(username)){}

    void WsConnection::start(){
        reactor_.addFd(fd_, EPOLLIN,
            [this](){ onRead(); },
            [this](){ onWrite(); });
    }

    // =========================================================================
    // onRead —— 循环 recv + 循环 tryDecode
    //   每解出一帧就分发:
    //     TEXT → onText_ 回调
    //     PING → 回 PONG（自动行为,不回调业务）
    //     PONG → 忽略（响应客户端的 ping）
    //     CLOSE → 发 close 帧 ack + 关闭
    //     BINARY → 拒绝（demo 不支持）
    //     CONTINUATION → 累积到 pendingFragment_（demo 简化忽略 fragmentation）
    // =========================================================================
    void WsConnection::onRead(){
        char tmp[4096];
        while (true){
            ssize_t n = ::recv(fd_, tmp, sizeof(tmp), MSG_DONTWAIT);
            if (n > 0){
                readBuf_.append(reinterpret_cast<const uint8_t*>(tmp), size_t(n));
            } else if (n == 0){
                close();          // 对端关连接
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                close();          // 真错误
                return;
            }
        }

        // 循环解帧
        while (true){
            // TODO: 实现 onRead 的解帧循环
            //
            // 步骤：
            //   1. if (readBuf_.readableBytes() < 2) break;    // 不足首部
            if(readBuf_.readableBytes()<2){break;}
            //   2. ws::DecodeResult r = ws::tryDecode(readBuf_.peek(), readBuf_.readableBytes());
            ws::DecodeResult r=ws::tryDecode(readBuf_.peek(),readBuf_.readableBytes());
            //   3. if (r.consumed == -1) break;                // 数据不够,等下次
            //      if (r.consumed == 0) { close(); return; }   // 协议错
            if(-1==r.consumed){break;}
            if(0==r.consumed){close(); return;}   // 协议错,不要再解后续帧
            //   4. readBuf_.consume(r.consumed);               // 推进读指针
            readBuf_.consume(r.consumed);
            
            //分发frame
            switch (r.frame.opcode){
            case ws::Opcode::TEXT:{
                if(onText_){
                    onText_(this, std::string(
                        (char*)r.frame.payload.data(), r.frame.payload.size()));
                }
                break;
            }
            case ws::Opcode::PING:{
                // RFC 6455 §5.5.3：收到 PING 必须回 PONG，payload 镜像回去
                auto pong = ws::encodePong(std::string(
                    (char*)r.frame.payload.data(), r.frame.payload.size()));
                writeBuf_.append(pong.data(),pong.size());
                if (!writing_){
                    reactor_.modifyFd(fd_, EPOLLIN | EPOLLOUT);
                    writing_ = true;
                }
                break;
            }
            case ws::Opcode::PONG:{
                // 我们是 PING 发起方,PONG 是对方的响应,啥也不做
                break;
            }
            case ws::Opcode::BINARY:{
                // 不支持二进制帧
                close();
                return;
            }
            case ws::Opcode::CLOSE:{
                // 发一个 close ack（code=1000 normal closure），等 writeBuf flush 完再关
                auto ack = ws::encodeClose();
                writeBuf_.append(ack.data(),ack.size());
                closeAfterWrite_ = true;
                if (!writing_){
                    reactor_.modifyFd(fd_, EPOLLIN | EPOLLOUT);
                    writing_ = true;
                }
                break;
            }
            default:{
                // 未知 opcode,协议错
                close();
                return;
            }
            }
        }
    }

    // =========================================================================
    // onWrite —— 把 writeBuf 写出去,写完清 EPOLLOUT
    //   如果 closeAfterWrite_ 为 true（来自 close 帧 ack 后）,写完就 close
    // =========================================================================
    void WsConnection::onWrite(){
        // TODO: 实现 onWrite
        while(writeBuf_.readableBytes()>0){
            ssize_t n=::send(fd_,writeBuf_.peek(),writeBuf_.readableBytes(),MSG_NOSIGNAL | MSG_DONTWAIT);
            if(n>0){
                writeBuf_.consume(n);
            }else if(n<0 && (errno == EAGAIN || errno == EWOULDBLOCK)){
                return;
            }else{
                close();
                return;
            }
        }
        if(0==writeBuf_.readableBytes()){
            if(closeAfterWrite_){
                close();
            }else{
                reactor_.modifyFd(fd_,EPOLLIN);
                writing_=false;
            }
        }
    }

    // =========================================================================
    // sendText —— 外部 API:编码 → writeBuf → 调度写
    // =========================================================================
    void WsConnection::sendText(const std::string& payload){
        if (!isActive()) return;
        auto bytes = ws::encodeText(payload);
        writeBuf_.append(bytes.data(),bytes.size());
        if (!writing_){
            reactor_.modifyFd(fd_, EPOLLIN | EPOLLOUT);
            writing_ = true;
        }
    }

    void WsConnection::sendPing(const std::string& payload){
        if (!isActive()) return;
        auto bytes = ws::encodePing(payload);
        writeBuf_.append(bytes.data(),bytes.size());
        if (!writing_){
            reactor_.modifyFd(fd_, EPOLLIN | EPOLLOUT);
            writing_ = true;
        }
    }

    // =========================================================================
    // close —— 幂等关闭
    // =========================================================================
    void WsConnection::close(){
        if(state_==CLOSED){return;}
        state_=CLOSED;
        reactor_.removeFd(fd_);
        if(onClose_){onClose_(this);}
        ::close(fd_);
    }

}
#pragma once

//传递给插件的调用接口
//add by freeeyes

#include "ISessionService.h"
#include "ModuleLogic.h"

class CSessionService : public ISessionService
{
public:
    void send_io_message(uint32 connect_id, CMessage_Packet send_packet) final;
    bool connect_io_server(const CConnect_IO_Info& io_info, EM_CONNECT_IO_TYPE io_type) final;
    void close_io_session(uint32 connect_id) final;
    bool add_session_io_mapping(_ClientIPInfo from_io, EM_CONNECT_IO_TYPE from_io_type, _ClientIPInfo to_io, EM_CONNECT_IO_TYPE to_io_type) final;
    bool delete_session_io_mapping(_ClientIPInfo from_io, EM_CONNECT_IO_TYPE from_io_type) final;
    bool send_frame_message(uint16 tag_thread_id, std::string message_tag, CMessage_Packet send_packet, std::chrono::seconds delay_seconds = std::chrono::seconds(0)) final;
    bool create_frame_work_thread(uint32 thread_id) final;
    uint16 get_io_work_thread_count() final;
    uint16 get_plugin_work_thread_count() final;
};

using App_SessionService = PSS_singleton<CSessionService>;

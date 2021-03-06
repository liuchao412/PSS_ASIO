#include "ModuleLogic.h"

void CModuleLogic::init_logic(command_to_module_function command_to_module_function, uint16 work_thread_id)
{
    modules_interface_.copy_from_module_list(command_to_module_function);
    work_thread_id_ = work_thread_id;
}

void CModuleLogic::add_session(uint32 connect_id, shared_ptr<ISession> session, const _ClientIPInfo& local_info, const _ClientIPInfo& romote_info)
{
    sessions_interface_.add_session_interface(connect_id, session, local_info, romote_info);
}

shared_ptr<ISession> CModuleLogic::get_session_interface(uint32 connect_id)
{
    work_thread_state_ = ENUM_WORK_THREAD_STATE::WORK_THREAD_BEGIN;
    auto ret = sessions_interface_.get_session_interface(connect_id);
    work_thread_state_ = ENUM_WORK_THREAD_STATE::WORK_THREAD_END;
    work_thread_run_time_ = std::chrono::steady_clock::now();
    return ret;
}

void CModuleLogic::delete_session_interface(uint32 connect_id)
{
    sessions_interface_.delete_session_interface(connect_id);
}

void CModuleLogic::close()
{
    modules_interface_.close();
    sessions_interface_.close();
}

int CModuleLogic::do_thread_module_logic(const CMessage_Source& source, const CMessage_Packet& recv_packet, CMessage_Packet& send_packet)
{
    return modules_interface_.do_module_message(source, recv_packet, send_packet);
}

uint16 CModuleLogic::get_work_thread_id()
{
    return work_thread_id_;
}

int CModuleLogic::get_work_thread_timeout()
{
    std::chrono::seconds time_interval(0);
    if(ENUM_WORK_THREAD_STATE::WORK_THREAD_BEGIN == work_thread_state_)
    {
        auto interval_seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - work_thread_run_time_);
        return (int)interval_seconds.count();
    }
    else
    {
        return 0;
    }
}

void CWorkThreadLogic::init_work_thread_logic(int thread_count, uint16 timeout_seconds, config_logic_list& logic_list, ISessionService* session_service)
{
    //初始化线程数
    thread_count_ = thread_count;

    App_tms::instance()->Init();

    load_module_.set_session_service(session_service);

    //初始化插件加载
    for (auto logic_library : logic_list)
    {
        load_module_.load_plugin_module(logic_library.logic_path_, 
            logic_library.logic_file_name_, 
            logic_library.logic_param_);
    }

    //执行线程对应创建
    for (int i = 0; i < thread_count; i++)
    {
        auto thread_logic = make_shared<CModuleLogic>();

        thread_logic->init_logic(load_module_.get_module_function_list(), i);

        thread_module_list_.emplace_back(thread_logic);

        //初始化线程
        App_tms::instance()->CreateLogic(i);
    }

    module_init_finish_ = true;

    //创建插件使用的线程
    for (auto thread_id : plugin_work_thread_buffer_list_)
    {
        //查找线程是否已经存在
        auto f = plugin_work_thread_list_.find(thread_id);
        if (f != plugin_work_thread_list_.end())
        {
            continue;
        }

        auto thread_logic = make_shared<CModuleLogic>();

        thread_logic->init_logic(load_module_.get_module_function_list(), thread_id);

        plugin_work_thread_list_[thread_id] = thread_logic;

        //初始化线程
        App_tms::instance()->CreateLogic(thread_id);
    }

    plugin_work_thread_buffer_list_.clear();

    //加载插件投递事件
    for (auto plugin_events : plugin_work_thread_buffer_message_list_)
    {
        send_frame_message(plugin_events.tag_thread_id_,
            plugin_events.message_tag_,
            plugin_events.send_packet_,
            plugin_events.delay_seconds_);

    }
    plugin_work_thread_buffer_message_list_.clear();

    //定时检查任务，定时检查服务器状态
    App_TimerManager::instance()->GetTimerPtr()->addTimer_loop(chrono::seconds(0), chrono::seconds(timeout_seconds), [this, timeout_seconds]()
        {
            run_check_task(timeout_seconds);
        });
    
}

void CWorkThreadLogic::init_communication_service(ICommunicationInterface* communicate_service)
{
    communicate_service_ = communicate_service;
}

void CWorkThreadLogic::close()
{
    //关闭线程操作
    App_tms::instance()->Close();

    communicate_service_->close();

    for (auto f : thread_module_list_)
    {
        f->close();
    }

    thread_module_list_.clear();

    //关闭模板操作
    load_module_.Close();
}

void CWorkThreadLogic::add_thread_session(uint32 connect_id, shared_ptr<ISession> session, _ClientIPInfo& local_info, const _ClientIPInfo& romote_info)
{
    //session 建立连接
    uint16 curr_thread_index = connect_id % thread_count_;
    auto module_logic = thread_module_list_[curr_thread_index];

    auto server_id = session->get_mark_id(connect_id);
    if (server_id > 0)
    {
        //关联服务器间链接
        communicate_service_->set_connect_id(server_id, connect_id);
    }

    //添加点对点映射
    io_to_io_.regedit_session_id(romote_info, session->get_io_type(), connect_id);

    //向插件告知链接建立消息
    App_tms::instance()->AddMessage(curr_thread_index, [session, connect_id, module_logic, local_info, romote_info]() {
        //PSS_LOGGER_DEBUG("[CTcpSession::AddMessage]count={}.", message_list.size());

        module_logic->add_session(connect_id, session, local_info, romote_info);

        CMessage_Source source;
        CMessage_Packet recv_packet;
        CMessage_Packet send_packet;

        recv_packet.command_id_ = LOGIC_COMMAND_CONNECT;

        source.connect_id_ = connect_id;
        source.work_thread_id_ = module_logic->get_work_thread_id();
        source.type_ = session->get_io_type();
        source.connect_mark_id_ = session->get_mark_id(connect_id);
        source.local_ip_ = local_info;
        source.remote_ip_ = romote_info;

        module_logic->do_thread_module_logic(source, recv_packet, send_packet);
        });
}

void CWorkThreadLogic::delete_thread_session(uint32 connect_id, _ClientIPInfo from_io, shared_ptr<ISession> session)
{
    //session 连接断开
    uint16 curr_thread_index = connect_id % thread_count_;
    auto module_logic = thread_module_list_[curr_thread_index];
    module_logic->delete_session_interface(connect_id);

    auto server_id = session->get_mark_id(connect_id);
    if (server_id > 0)
    {
        //取消服务器间链接
        communicate_service_->set_connect_id(server_id, 0);
    }

    //清除点对点转发消息映射
    io_to_io_.unregedit_session_id(from_io, session->get_io_type());

    //向插件告知链接建立消息
    App_tms::instance()->AddMessage(curr_thread_index, [session, connect_id, module_logic]() {
        //PSS_LOGGER_DEBUG("[CTcpSession::AddMessage]count={}.", message_list.size());
        CMessage_Source source;
        CMessage_Packet recv_packet;
        CMessage_Packet send_packet;

        recv_packet.command_id_ = LOGIC_COMMAND_DISCONNECT;

        source.connect_id_ = connect_id;
        source.work_thread_id_ = module_logic->get_work_thread_id();
        source.type_ = session->get_io_type();
        source.connect_mark_id_ = session->get_mark_id(connect_id);

        module_logic->do_thread_module_logic(source, recv_packet, send_packet);
        });
}

void CWorkThreadLogic::close_session_event(uint32 connect_id)
{
    //session 关闭事件分发
    uint16 curr_thread_index = connect_id % thread_count_;
    auto module_logic = thread_module_list_[curr_thread_index];

    App_tms::instance()->AddMessage(curr_thread_index, [module_logic, connect_id]() {
        auto session = module_logic->get_session_interface(connect_id);
        if (session != nullptr)
        {
            session->close(connect_id);
        }
        module_logic->delete_session_interface(connect_id);
        });
}

int CWorkThreadLogic::do_thread_module_logic(const uint32 connect_id, vector<CMessage_Packet>& message_list, shared_ptr<ISession> session)
{
    //处理线程的投递
    uint16 curr_thread_index = connect_id % thread_count_;
    auto module_logic = thread_module_list_[curr_thread_index];

    auto io_2_io_session_id = io_to_io_.get_to_session_id(connect_id);
    if (io_2_io_session_id > 0)
    {
        curr_thread_index = io_2_io_session_id % thread_count_;
        auto module_logic = thread_module_list_[io_2_io_session_id];

        //存在点对点透传，直接透传数据
        App_tms::instance()->AddMessage(curr_thread_index, [session, io_2_io_session_id, message_list, module_logic]() {
            for (auto recv_packet : message_list)
            {
                auto session = module_logic->get_session_interface(io_2_io_session_id);
                if (nullptr != session)
                {
                    session->do_write_immediately(io_2_io_session_id, recv_packet.buffer_.c_str(), recv_packet.buffer_.size());
                }
            }
        });
    }
    else
    {
        //添加到数据队列处理
        App_tms::instance()->AddMessage(curr_thread_index, [session, connect_id, message_list, module_logic]() {
            //PSS_LOGGER_DEBUG("[CTcpSession::AddMessage]count={}.", message_list.size());
            CMessage_Source source;
            CMessage_Packet send_packet;

            source.connect_id_ = connect_id;
            source.work_thread_id_ = module_logic->get_work_thread_id();
            source.type_ = session->get_io_type();
            source.connect_mark_id_ = session->get_mark_id(connect_id);

            for (auto recv_packet : message_list)
            {
                module_logic->do_thread_module_logic(source, recv_packet, send_packet);
            }

            if (send_packet.buffer_.size() > 0)
            {
                //有需要发送的内容
                session->set_write_buffer(connect_id, send_packet.buffer_.c_str(), send_packet.buffer_.size());
                session->do_write(connect_id);
            }
            });
    }

    return 0;
}

void CWorkThreadLogic::do_plugin_thread_module_logic(shared_ptr<CModuleLogic> module_logic, std::string message_tag, CMessage_Packet recv_packet)
{
    //添加到数据队列处理
    App_tms::instance()->AddMessage(module_logic->get_work_thread_id(), [message_tag, recv_packet, module_logic]() {
        //PSS_LOGGER_DEBUG("[CTcpSession::AddMessage]count={}.", message_list.size());
        CMessage_Source source;
        CMessage_Packet send_packet;

        source.work_thread_id_ = module_logic->get_work_thread_id();
        source.remote_ip_.m_strClientIP = message_tag;
        source.type_ = EM_CONNECT_IO_TYPE::CONNECT_IO_FRAME;

        module_logic->do_thread_module_logic(source, recv_packet, send_packet);

        //内部模块回调不在处理 send_packet 部分。

        });
}

bool CWorkThreadLogic::create_frame_work_thread(uint32 thread_id)
{
    if (thread_id < thread_count_)
    {
        PSS_LOGGER_DEBUG("[CWorkThreadLogic::create_frame_work_thread]thread id must more than config thread count.");
        return false;
    }

    if (false == module_init_finish_)
    {
        //如果模块还没全部启动完毕，将这个创建线程的过程，放入vector里面，等模块全部加载完毕，启动。
        plugin_work_thread_buffer_list_.emplace_back(thread_id);
    }
    else
    {
        //查找这个线程ID是否已经存在了
        auto f = plugin_work_thread_list_.find(thread_id);
        if (f != plugin_work_thread_list_.end())
        {
            PSS_LOGGER_DEBUG("[CWorkThreadLogic::create_frame_work_thread]thread id already exist.");
            return false;
        }

        //创建线程
        auto thread_logic = make_shared<CModuleLogic>();

        thread_logic->init_logic(load_module_.get_module_function_list(), thread_id);

        plugin_work_thread_list_[thread_id] = thread_logic;

        //初始化线程
        App_tms::instance()->CreateLogic(thread_id);
    }

    return true;
}

uint16 CWorkThreadLogic::get_io_work_thread_count()
{
    return thread_count_;
}

uint16 CWorkThreadLogic::get_plugin_work_thread_count()
{
    return (uint16)plugin_work_thread_list_.size();
}

void CWorkThreadLogic::send_io_message(uint32 connect_id, CMessage_Packet send_packet)
{
    //处理线程的投递
    uint16 curr_thread_index = connect_id % thread_count_;
    auto module_logic = thread_module_list_[curr_thread_index];

    //添加到数据队列处理
    App_tms::instance()->AddMessage(curr_thread_index, [this, connect_id, send_packet, module_logic]() {
        if (nullptr != module_logic->get_session_interface(connect_id))
        {
            module_logic->get_session_interface(connect_id)->do_write_immediately(connect_id,
                send_packet.buffer_.c_str(),
                send_packet.buffer_.size());
        }
        else
        {
            //查找是不是服务器间链接，如果是，则调用重连。
            auto server_id = communicate_service_->get_server_id(connect_id);
            if (server_id > 0)
            {
                //重连服务器
                communicate_service_->reset_connect(server_id);
            }
        }
        });
}

bool CWorkThreadLogic::connect_io_server(const CConnect_IO_Info& io_info, EM_CONNECT_IO_TYPE io_type)
{
    //寻找当前server_id是否存在
    if (true == communicate_service_->is_exist(io_info.server_id))
    {
        PSS_LOGGER_DEBUG("[CWorkThreadLogic::connect_io_server]server_id={0} is exist.");
        return false;
    }
    else
    {
        return communicate_service_->add_connect(io_info, io_type);
    }
}

void CWorkThreadLogic::close_io_server(uint32 server_id)
{
    communicate_service_->close_connect(server_id);
}

uint32 CWorkThreadLogic::get_io_server_id(uint32 connect_id)
{
    return communicate_service_->get_server_id(connect_id);
}

bool CWorkThreadLogic::add_session_io_mapping(_ClientIPInfo from_io, EM_CONNECT_IO_TYPE from_io_type, _ClientIPInfo to_io, EM_CONNECT_IO_TYPE to_io_type)
{
    return io_to_io_.add_session_io_mapping(from_io, from_io_type, to_io, to_io_type);
}

bool CWorkThreadLogic::delete_session_io_mapping(_ClientIPInfo from_io, EM_CONNECT_IO_TYPE from_io_type)
{
    return io_to_io_.delete_session_io_mapping(from_io, from_io_type);
}

void CWorkThreadLogic::run_check_task(uint32 timeout_seconds)
{
    for (auto module_logic : thread_module_list_)
    {
        auto work_thread_timeout = module_logic->get_work_thread_timeout();
        if (work_thread_timeout > (int)timeout_seconds)
        {
            PSS_LOGGER_ERROR("[CWorkThreadLogic::run_check_task]work thread{0} is block.", module_logic->get_work_thread_id());
        }
    }

    PSS_LOGGER_DEBUG("[CWorkThreadLogic::run_check_task]check is ok.");
}

bool CWorkThreadLogic::send_frame_message(uint16 tag_thread_id, std::string message_tag, CMessage_Packet send_packet, std::chrono::seconds delay_seconds)
{
    if (false == module_init_finish_)
    {
        CDelayPluginMessage plugin_message;
        plugin_message.tag_thread_id_ = tag_thread_id;
        plugin_message.message_tag_ = message_tag;
        plugin_message.send_packet_ = send_packet;
        plugin_message.delay_seconds_ = delay_seconds;
        plugin_work_thread_buffer_message_list_.emplace_back(plugin_message);
        return true;
    }

    auto f = plugin_work_thread_list_.find(tag_thread_id);
    if (f == plugin_work_thread_list_.end())
    {
        return false;
    }

    auto plugin_thread = f->second;

    if (delay_seconds == std::chrono::seconds(0))
    {
        //不需要延时，立刻投递
        do_plugin_thread_module_logic(plugin_thread, message_tag, send_packet);
    }
    else
    {
        //需要延时，延时后投递
        App_TimerManager::instance()->GetTimerPtr()->addTimer(delay_seconds, [this, plugin_thread, message_tag, send_packet]()
            {
                //延时到期，进行投递
                do_plugin_thread_module_logic(plugin_thread, message_tag, send_packet);
            });
    }

    return true;
}


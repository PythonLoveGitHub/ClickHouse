#pragma once

#include <Core/Names.h>
#include <Core/Types.h>
#include <IO/ReadBuffer.h>
#include <amqpcpp.h>
#include <Storages/RabbitMQ/RabbitMQHandler.h>
#include <Core/BackgroundSchedulePool.h>
#include <event2/event.h>

namespace Poco
{
    class Logger;
}

namespace DB
{

using ChannelPtr = std::shared_ptr<AMQP::TcpChannel>;
using HandlerPtr = std::shared_ptr<RabbitMQHandler>;

class ReadBufferFromRabbitMQConsumer : public ReadBuffer
{

public:
    ReadBufferFromRabbitMQConsumer(
            ChannelPtr consumer_channel_,
            HandlerPtr event_handler_,
            const String & exchange_name_,
            const Names & routing_keys_,
            const size_t channel_id_,
            Poco::Logger * log_,
            char row_delimiter_,
            const bool bind_by_id_,
            const size_t num_queues_,
            const String & exchange_type_,
            const String & local_exchange_,
            const std::atomic<bool> & stopped_);

    ~ReadBufferFromRabbitMQConsumer() override;

    void allowNext() { allowed = true; } // Allow to read next message.
    void checkSubscription();

    auto getExchange() const { return exchange_name; }

private:
    using Messages = std::vector<String>;

    ChannelPtr consumer_channel;
    HandlerPtr event_handler;

    const String & exchange_name;
    const Names & routing_keys;
    const size_t channel_id;
    const bool bind_by_id;
    const size_t num_queues;

    const String & exchange_type;
    const String & local_exchange;
    const String local_default_exchange;
    const String local_hash_exchange;

    Poco::Logger * log;
    char row_delimiter;
    bool stalled = false;
    bool allowed = true;
    const std::atomic<bool> & stopped;

    String default_local_exchange;
    bool local_exchange_declared = false, local_hash_exchange_declared = false;
    bool exchange_type_set = false, hash_exchange = false;

    std::atomic<bool> loop_started = false, consumer_error = false;
    std::atomic<size_t> count_subscribed = 0, wait_subscribed;

    std::vector<String> queues;
    Messages received;
    Messages messages;
    Messages::iterator current;
    std::unordered_map<String, bool> subscribed_queue;

    /* Note: as all consumers share the same connection => they also share the same
     * event loop, which can be started by any consumer and the loop is blocking only to the thread that
     * started it, and the loop executes ALL active callbacks on the connection => in case num_consumers > 1,
     * at most two threads will be present: main thread and the one that executes callbacks (1 thread if
     * main thread is the one that started the loop).
     */
    std::mutex mutex;

    bool nextImpl() override;

    void initExchange();
    void initQueueBindings(const size_t queue_id);
    void subscribe(const String & queue_name);
    void startEventLoop(std::atomic<bool> & loop_started);
    void stopEventLoop();

};
}

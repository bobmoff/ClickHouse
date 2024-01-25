#include <IO/WriteBufferFromFile.h>
#include <IO/HTTPCommon.h>
#include <Common/ConnectionPool.h>

#include <Poco/URI.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/MessageHeader.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>

#include <base/sleep.h>

#include <thread>
#include <gtest/gtest.h>

size_t stream_copy_n(std::istream & in, std::ostream & out, std::size_t count = std::numeric_limits<size_t>::max())
{
    const size_t buffer_size = 4096;
    char buffer[buffer_size];

    size_t total_read = 0;

    while (count > buffer_size)
    {
        in.read(buffer, buffer_size);
        size_t read = in.gcount();
        out.write(buffer, read);
        count -= read;
        total_read += read;

        if (read == 0)
            return total_read;
    }

    in.read(buffer, count);
    size_t read = in.gcount();
    out.write(buffer, read);
    total_read += read;

    return total_read;
}

class MockRequestHandler : public Poco::Net::HTTPRequestHandler
{
public:
    MockRequestHandler(DB::ConnectionTimeouts & timeouts)
        :slowdown(timeouts)
    {
    }

    void handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response) override
    {
        std::cerr << "handler\n";
        response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
        size_t size = request.getContentLength();
        response.setContentLength(size); // ContentLength is required for keep alive

        sleepForMicroseconds(slowdown.receive_timeout.totalMicroseconds());

        std::cerr << "handler write: " << size << "\n";
        size_t copied = stream_copy_n(request.stream(), response.send(), size);
        std::cerr << "handleRequest: " << copied << "/" << size  << std::endl;
    }

    DB::ConnectionTimeouts & slowdown;
};

class HTTPRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
{
public:
    HTTPRequestHandlerFactory(DB::ConnectionTimeouts & timeouts)
        :slowdown(timeouts)
    {
    }

    virtual Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &) override
    {
        return new MockRequestHandler(slowdown);
    }

    DB::ConnectionTimeouts & slowdown;
};

using HTTPSession = Poco::Net::HTTPClientSession;
using HTTPSessionPtr = std::shared_ptr<Poco::Net::HTTPClientSession>;

class ConnectionPoolTest : public testing::Test {
protected:
    ConnectionPoolTest() = default;

    // If the constructor and destructor are not enough for setting up
    // and cleaning up each test, you can define the following methods:

    void SetUp() override {
        timeouts = DB::ConnectionTimeouts();
        slowdown_timeouts = DB::ConnectionTimeouts()
                            .withConnectionTimeout(0)
                            .withReceiveTimeout(0)
                            .withSendTimeout(0)
                            .withHttpKeepAliveTimeout(0);

        DB::ConnectionPools::instance().clear();
        DB::CurrentThread::getProfileEvents().reset();
        startServer();
        // Code here will be called immediately after the constructor (right
        // before each test).
    }

    void TearDown() override {
        // Code here will be called immediately after each test (right
        // before the destructor).
    }

    DB::IEndpointConnectionPool::Ptr getPool()
    {
        auto uri = Poco::URI(getServerUrl());
        return DB::ConnectionPools::instance().getPool(uri, DB::ProxyConfiguration{});
    }

    std::string getServerUrl()
    {
        return "http://" + server_data.socket->address().toString();
    }

    void startServer()
    {
        server_data = {};
        server_data.params = new Poco::Net::HTTPServerParams();
        server_data.handler_factory = new HTTPRequestHandlerFactory(slowdown_timeouts);
        server_data.socket.reset(new Poco::Net::ServerSocket(server_data.port));
        server_data.server.reset(
            new Poco::Net::HTTPServer(server_data.handler_factory, *server_data.socket, server_data.params));

        server_data.server->start();
    }

    Poco::Net::HTTPServer & getServer()
    {
        return *server_data.server;
    }

    struct
    {
        // just some port to avoid collisions with others tests
        UInt16 port = 9871;
        Poco::Net::HTTPServerParams::Ptr params;
        HTTPRequestHandlerFactory::Ptr handler_factory;
        std::unique_ptr<Poco::Net::ServerSocket> socket;
        std::unique_ptr<Poco::Net::HTTPServer> server;
    } server_data;

    DB::ConnectionPoolMetrics metrics = DB::IEndpointConnectionPool::getMetrics(DB::MetricsType::METRICS_FOR_HTTP);
    DB::ConnectionTimeouts timeouts;
    DB::ConnectionTimeouts slowdown_timeouts;
};


void wait_until(std::function<bool()> pred)
{
    while (!pred())
        Poco::Thread::sleep(250);
}

void echoRequest(String data, HTTPSession & session)
{
    {
        Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_PUT, "/", "HTTP/1.1"); // HTTP/1.1 is required for keep alive
        request.setContentLength(data.size());
        std::cerr << "send\n";
        std::ostream & ostream = session.sendRequest(request);
        std::cerr << "write\n";
        ostream << data;
    }

    {
        std::stringstream result;
        Poco::Net::HTTPResponse response;
        std::cerr << "receive\n";
        std::istream & istream = session.receiveResponse(response);
        ASSERT_EQ(response.getStatus(), Poco::Net::HTTPResponse::HTTP_OK);

        std::cerr << "read\n";
        stream_copy_n(istream, result);
        ASSERT_EQ(data, result.str());
    }
}

TEST_F(ConnectionPoolTest, CanConnect)
{
    auto pool = getPool();
    auto connection = pool->getConnection(timeouts);

    ASSERT_TRUE(connection->connected());
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));

    wait_until([&] () { return getServer().currentConnections() == 1; });
    ASSERT_EQ(1, getServer().currentConnections());
    ASSERT_EQ(1, getServer().totalConnections());

    connection->reset();

    wait_until([&] () { return getServer().currentConnections() == 0; });
    ASSERT_EQ(0, getServer().currentConnections());
    ASSERT_EQ(1, getServer().totalConnections());

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
}

TEST_F(ConnectionPoolTest, CanRequest)
{
    auto pool = getPool();
    auto connection = pool->getConnection(timeouts);

    std::cerr << "request\n";
    echoRequest("Hello", *connection);

    ASSERT_EQ(1, getServer().totalConnections());
    ASSERT_EQ(1, getServer().currentConnections());

    std::cerr << "reset\n";
    connection->reset();

    std::cerr << "wait\n";
    wait_until([&] () { return getServer().currentConnections() == 0; });
    ASSERT_EQ(0, getServer().currentConnections());
    ASSERT_EQ(1, getServer().totalConnections());

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
}

TEST_F(ConnectionPoolTest, CanPreserve)
{
    auto pool = getPool();

    {
        auto connection = pool->getConnection(timeouts);
        // DB::setReuseTag(*connection);
        std::cerr << "implicit save connection with reuse tag" << std::endl;
    }

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));

    wait_until([&] () { return getServer().currentConnections() == 1; });
    ASSERT_EQ(1, getServer().currentConnections());

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
}

TEST_F(ConnectionPoolTest, CanReuse)
{
    auto pool = getPool();

    {
        auto connection = pool->getConnection(timeouts);
        // DB::setReuseTag(*connection);
    }

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));

    {
        auto connection = pool->getConnection(timeouts);

        ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
        ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));

        wait_until([&] () { return getServer().currentConnections() == 1; });
        ASSERT_EQ(1, getServer().currentConnections());

        echoRequest("Hello", *connection);

        ASSERT_EQ(1, getServer().totalConnections());
        ASSERT_EQ(1, getServer().currentConnections());

        std::cerr << "explicit reset connection" << std::endl;
        connection->reset();
    }

    ASSERT_EQ(0, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));

    wait_until([&] () { return getServer().currentConnections() == 0; });
    ASSERT_EQ(0, getServer().currentConnections());
    ASSERT_EQ(1, getServer().totalConnections());

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reused]);
}

TEST_F(ConnectionPoolTest, CanReuse10)
{
    auto pool = getPool();


    for (int i = 0; i < 10; ++i)
    {
        auto connection = pool->getConnection(timeouts);
        echoRequest("Hello", *connection);
    }

    {
        auto connection = pool->getConnection(timeouts);
        connection->reset(); // reset just not to wait its expiration here
    }

    wait_until([&] () { return getServer().currentConnections() == 0; });
    ASSERT_EQ(0, getServer().currentConnections());
    ASSERT_EQ(1, getServer().totalConnections());

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(10, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(10, DB::CurrentThread::getProfileEvents()[metrics.reused]);
}

TEST_F(ConnectionPoolTest, CanReuse5)
{
    timeouts.withHttpKeepAliveTimeout(1);

    auto pool = getPool();

    std::vector<DB::HTTPSessionPtr> connections;
    for (int i = 0; i < 5; ++i)
    {
        connections.push_back(pool->getConnection(timeouts));
    }
    connections.clear();

    ASSERT_EQ(5, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(5, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(5, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(5, CurrentMetrics::get(metrics.stored_count));

    wait_until([&] () { return getServer().currentConnections() == 5; });
    ASSERT_EQ(5, getServer().currentConnections());
    ASSERT_EQ(5, getServer().totalConnections());

    for (int i = 0; i < 5; ++i)
    {
        auto connection = pool->getConnection(timeouts);
        echoRequest("Hello", *connection);
    }

    ASSERT_EQ(5, getServer().totalConnections());

    ASSERT_EQ(5, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(10, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(5, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(5, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(5, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, CanReconnectAndCreate)
{
    auto pool = getPool();

    std::vector<HTTPSessionPtr> in_use;

    const size_t count = 2;
    for (int i = 0; i < count; ++i)
    {
        auto connection = pool->getConnection(timeouts);
        // DB::setReuseTag(*connection);
        in_use.push_back(connection);
    }

    ASSERT_EQ(count, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);

    ASSERT_EQ(count, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));

    auto connection = std::move(in_use.back());
    in_use.pop_back();

    echoRequest("Hello", *connection);

    connection->abort(); // further usage requires reconnect, new connection

    echoRequest("Hello", *connection);

    connection->reset();

    wait_until([&] () { return getServer().currentConnections() == 1; });
    ASSERT_EQ(1, getServer().currentConnections());
    ASSERT_EQ(count+1, getServer().totalConnections());

    ASSERT_EQ(count+1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
}

TEST_F(ConnectionPoolTest, CanReconnectAndReuse)
{
    auto pool = getPool();

    std::vector<HTTPSessionPtr> in_use;

    const size_t count = 2;
    for (int i = 0; i < count; ++i)
    {
        auto connection = pool->getConnection(timeouts);
        // DB::setReuseTag(*connection);
        in_use.push_back(std::move(connection));
    }

    auto connection = std::move(in_use.back());
    in_use.pop_back();
    in_use.clear(); // other connection will be reused

    echoRequest("Hello", *connection);

    connection->abort(); // further usage requires reconnect, reuse connection from pool

    echoRequest("Hello", *connection);

    connection->reset();

    wait_until([&] () { return getServer().currentConnections() == 0; });
    ASSERT_EQ(0, getServer().currentConnections());
    ASSERT_EQ(2, getServer().totalConnections());

    ASSERT_EQ(count, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reused]);
}

TEST_F(ConnectionPoolTest, ReceiveTimeout)
{
    slowdown_timeouts.withReceiveTimeout(2);
    timeouts.withReceiveTimeout(1);

    startServer();

    auto pool = getPool();

    {
        auto connection = pool->getConnection(timeouts);
        ASSERT_ANY_THROW(
            echoRequest("Hello", *connection);
        );
    }

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);

    {
        timeouts.withReceiveTimeout(3);
        auto connection = pool->getConnection(timeouts);
        ASSERT_NO_THROW(
            echoRequest("Hello", *connection);
        );
    }

    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);

    {
        /// timeouts have effect for reused session
        timeouts.withReceiveTimeout(1);
        auto connection = pool->getConnection(timeouts);
        ASSERT_ANY_THROW(
            echoRequest("Hello", *connection);
        );
    }

    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.reset]);
}

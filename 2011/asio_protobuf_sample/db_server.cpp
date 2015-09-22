//
// db_server.cpp: DbServer implementation
//
// Eli Bendersky (eliben@gmail.com)
// This code is in the public domain
//
#include "db_server.h"
#include "packedmessage.h"
#include "stringdb.pb.h"
#include "ocrRequest.pb.h"
#include "ocrResponse.pb.h"
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <sstream>
#include <vector>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/cstdint.hpp>
#include <boost/enable_shared_from_this.hpp>

using namespace std;
namespace asio = boost::asio;
using asio::ip::tcp;
using boost::uint8_t;


#define DEBUG true


typedef map<string, string> StringDatabase;


// Database connection - handles a connection with a single client.
// Create only through the DbConnection::create factory.
//
class DbConnection : public boost::enable_shared_from_this<DbConnection>
{
public:
    typedef boost::shared_ptr<DbConnection> Pointer;
    typedef boost::shared_ptr<hanvon::OCRRequest> RequestPointer;
    typedef boost::shared_ptr<hanvon::OCRResponse> ResponsePointer;
    typedef boost::shared_ptr<hanvon::ResultsMap>  ResultMapPointer;

    static Pointer create(asio::io_service& io_service, StringDatabase& db)
    {
        return Pointer(new DbConnection(io_service, db));
    }

    tcp::socket& get_socket()
    {
        return m_socket;
    }

    void start()
    {
        start_read_header();
    }

private:
    tcp::socket m_socket;
    StringDatabase& m_db_ref;
    vector<uint8_t> m_readbuf;
    PackedMessage<hanvon::OCRRequest> m_packed_request;

    DbConnection(asio::io_service& io_service, StringDatabase& db)
        : m_socket(io_service), m_db_ref(db),
        m_packed_request(boost::shared_ptr<hanvon::OCRRequest>(new hanvon::OCRRequest()))
    {
    }
    
    void handle_read_header(const boost::system::error_code& error)
    {
        DEBUG && (cerr << "handle read " << error.message() << '\n');
        if (!error) {
            DEBUG && (cerr << "Got header!\n");
            DEBUG && (cerr << show_hex(m_readbuf) << endl);
            unsigned msg_len = m_packed_request.decode_header(m_readbuf);
            DEBUG && (cerr << msg_len << " bytes\n");
            start_read_body(msg_len);
        }
    }

    void handle_read_body(const boost::system::error_code& error)
    {
        DEBUG && (cerr << "handle body " << error << '\n');
        if (!error) {
            DEBUG && (cerr << "Got body!\n");
            DEBUG && (cerr << show_hex(m_readbuf) << endl);
            handle_request();
            start_read_header();
        }
    }

    // Called when enough data was read into m_readbuf for a complete request
    // message. 
    // Parse the request, execute it and send back a response.
    //
    void handle_request()
    {
        if (m_packed_request.unpack(m_readbuf)) {
            RequestPointer req = m_packed_request.get_msg();
            ResponsePointer resp = prepare_response(req);
            
            vector<uint8_t> writebuf;
            PackedMessage<hanvon::OCRResponse> resp_msg(resp);
            resp_msg.pack(writebuf);
            asio::async_write(m_socket, asio::buffer(writebuf), boost::bind(&DbConnection::handle_write, shared_from_this(),
                    asio::placeholders::error));
            //asio::write(m_socket, asio::buffer(writebuf));

        }
    }

    void handle_write(const boost::system::error_code &ec)
    {
      if (!ec)
        m_socket.shutdown(tcp::socket::shutdown_send);
    }

    void start_read_header()
    {
        m_readbuf.resize(HEADER_SIZE);
        asio::async_read(m_socket, asio::buffer(m_readbuf),
                boost::bind(&DbConnection::handle_read_header, shared_from_this(),
                    asio::placeholders::error));
    }

    void start_read_body(unsigned msg_len)
    {
        // m_readbuf already contains the header in its first HEADER_SIZE
        // bytes. Expand it to fit in the body as well, and start async
        // read into the body.
        //
        m_readbuf.resize(HEADER_SIZE + msg_len);
        asio::mutable_buffers_1 buf = asio::buffer(&m_readbuf[HEADER_SIZE], msg_len);
        asio::async_read(m_socket, buf,
                boost::bind(&DbConnection::handle_read_body, shared_from_this(),
                    asio::placeholders::error));
    }

    ResponsePointer prepare_response(RequestPointer req)
    {
        string value;
        string image = req->image();
        DEBUG && (cerr << "received image: " << image << '\n');

        DEBUG && (cerr << "image length: " << image.length() << '\n');
        DEBUG && (cerr << "sender: " << req->sendername() << '\n');
        DEBUG && (cerr << "seq num:  " << req->seqnum() << '\n');

        for(int i=0;i<image.length(); i++){
        	 DEBUG && (cerr << "image byte: " << i  << image[i] << '\n');
        }

        DEBUG && (cerr << "prepare response\n");

        ResponsePointer resp(new hanvon::OCRResponse);
        //ResultMapPointer map = ResultMapPointer(new hanvon::ResultsMap());
        hanvon::ResultsMap* map = new hanvon::ResultsMap();

        resp->set_allocated_results(map);

        hanvon::MapEntry* entry = map->add_mapentry();
        entry->set_key(string("equation result 1"));
        entry->set_value(string("${|\sqrt [L]{(x+2)^{2}+y^{2}}-\sqrt [L]{(x-2)^{2}+y^{2}}|=2}$可化成${x^{2}-\\frac {y^{2}}{3}=1}$的形式."));

        google::protobuf::int32 seq = 9999;
        resp->set_seqnum(seq);
        resp->set_error("error message generated!!!产生错误信息！");
        return resp;
    }
};


struct DbServer::DbServerImpl
{
    tcp::acceptor acceptor;
    StringDatabase db;

    DbServerImpl(asio::io_service& io_service, unsigned port)
        : acceptor(io_service, tcp::endpoint(tcp::v4(), port))
    {
        start_accept();
    }

    void start_accept()
    {
        // Create a new connection to handle a client. Passing a reference
        // to db to each connection poses no problem since the server is 
        // single-threaded.
        //
        DbConnection::Pointer new_connection = 
            DbConnection::create(acceptor.get_io_service(), db);

        // Asynchronously wait to accept a new client
        //
        acceptor.async_accept(new_connection->get_socket(),
            boost::bind(&DbServerImpl::handle_accept, this, new_connection,
                asio::placeholders::error));
    }

    void handle_accept(DbConnection::Pointer connection,
            const boost::system::error_code& error)
    {
        // A new client has connected
        //
        if (!error) {
            // Start the connection
            //
            connection->start();

            // Accept another client
            //
            start_accept();
        }
    }
};


DbServer::DbServer(asio::io_service& io_service, unsigned port)
    : d(new DbServerImpl(io_service, port))
{
}


DbServer::~DbServer()
{
}



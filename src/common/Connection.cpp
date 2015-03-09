#include "Connection.h"

Connection::Connection(conn_id_t conn_id, asio::ip::tcp::socket socket)
    : conn_id(conn_id),
      header(3),
      payload(1024),
      socket(std::move(socket)) {


}

conn_id_t Connection::get_id() {
    return conn_id;
}

void Connection::read(ReadCommandHandler handler) {
    auto read_header_callback = [this, handler](int package_size) {
        payload.resize(package_size - 3);
        asio::async_read(socket, asio::buffer(payload), get_read_callback(handler, package_size));
    };
    read_lock.lock();
    asio::async_read(socket, asio::buffer(header), get_read_header_callback(read_header_callback));
}

ReadCallback Connection::get_read_callback(ReadCommandHandler handler, int package_size) {
    return [this, handler, package_size](const boost::system::error_code& err_code, std::size_t bytes_read) {
        std::cout << "read " << bytes_read << " from client, err_code: " << err_code << ", package_size: " << package_size << std::endl;
        if(!err_code && bytes_read >= package_size - 3) {
            std::vector<unsigned char> command(header.begin(), header.end());
            command.insert(command.end(), payload.begin(), payload.end());
            auto& network_command = network_package_manager.decode_command(command);
            handler(network_command);
        } else {
            disconnect();
        }
        read_lock.unlock();
    };
}

ReadCallback Connection::get_read_header_callback(ReadHeaderCommandHandler handler) {
    return [this, handler](const boost::system::error_code& err_code, std::size_t bytes_read) {
        std::cout << "read " << bytes_read << " from client, err_code: " << err_code << std::endl;
        if(!err_code && bytes_read == 3) {
            int package_size = network_package_manager.get_package_size(header);
            std::cout << "read header, package of size " << package_size << " incoming." << std::endl;
            if(package_size != -1) {
                handler(package_size);
            }
        } else {
            disconnect();
        }
    };
}

void Connection::write(NetworkPackage& command) {
    auto data = network_package_manager.encode_command(command);
    asio::async_write(socket, asio::buffer(data), [this](const boost::system::error_code& err_code, std::size_t bytes_written) {
    });
}

bool Connection::is_connected() {
    return socket.is_open();
}

void Connection::disconnect() {
    boost::system::error_code ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket.close();
}
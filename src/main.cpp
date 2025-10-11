// Basic DNS server that forwards queries to an upstream resolver
// Listens on UDP port 2053
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <bits/stdc++.h>

std::vector<unsigned char> parse_qname(const unsigned char *buffer, int &offset){
    std::vector<unsigned char> qname;
    int jumped = -1;
    int pos = offset;

    while (true){
        unsigned char len = buffer[pos];
        if ((len & 0xC0) == 0xC0){   // 0xC0 -> 1100 0000
            // pointer
            uint16_t pointer;
            std::memcpy(&pointer, buffer + pos, 2);
            pointer = ntohs(pointer);
            pointer &= 0x3FFF;  // 0x3FFF -> 0011 1111 1111 1111

            if (jumped == -1)
                offset = pos + 2;
            pos = pointer;
        }else if (len == 0){
            qname.push_back(0);
            if (jumped == -1)
                offset = pos + 1;
            break;
        }else{
            qname.push_back(len);
            pos++;
            for (int i = 0; i < len; i++){
                qname.push_back(buffer[pos]);
                pos++;
            }
        }
    }
    return qname;
}
int main(int argc, char *argv[]){

    if (argc != 3 || std::string(argv[1]) != "--resolver"){
        std::cerr << "Usage: ./your_server --resolver <ip>:<port>\n";
        return 1;
    }

    std::string resolver_addr(argv[2]);
    auto pos = resolver_addr.find(':');
    if (pos == std::string::npos){
        std::cerr << "Resolver must be in format <ip>:<port>\n";
        return 1;
    }
    std::string resolver_ip = resolver_addr.substr(0, pos);
    uint16_t resolver_port = std::stoi(resolver_addr.substr(pos + 1));

    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    setbuf(stdout, NULL);

    std::cout << "Logs from your program will appear here!" << std::endl;

    int udpSocket;    // This is the main server socket. It will listen for queries from clients.
    struct sockaddr_in clientAddress;

    udpSocket = socket(AF_INET, SOCK_DGRAM, 0);  

    if (udpSocket == -1){
        std::cerr << "Socket creation failed: " << strerror(errno) << "..." << std::endl;
        return 1;
    }

    // Since the tester restarts your program quite often, setting REUSE_PORT
    // ensures that we don't run into 'Address already in use' errors
    int reuse = 1;
    if (setsockopt(udpSocket, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0){  
        std::cerr << "SO_REUSEPORT failed: " << strerror(errno) << std::endl;
        return 1;
    }

    sockaddr_in serv_addr = {         
        .sin_family = AF_INET,
        .sin_port = htons(2053),
        .sin_addr = {htonl(INADDR_ANY)},
    };

    if (bind(udpSocket, reinterpret_cast<struct sockaddr *>(&serv_addr), sizeof(serv_addr)) != 0){  // The bind function associates udpSocket with a specific IP address and port.
        std::cerr << "Bind failed: " << strerror(errno) << std::endl;
        return 1;
    }

    int forwardSocket = socket(AF_INET, SOCK_DGRAM, 0); // This socket is used to send queries to the upstream resolver and receive answers from it.
    if (forwardSocket == -1){
        perror("Forward socket creation failed");
        return 1;
    }

    sockaddr_in resolver;
    resolver.sin_family = AF_INET;
    resolver.sin_port = htons(resolver_port);
    inet_pton(AF_INET, resolver_ip.c_str(), &resolver.sin_addr);

    int bytesRead;
    char buffer[512];
    socklen_t clientAddrLen = sizeof(clientAddress);

    while (true){
        // Receive data
        // The program blocks here, waiting for a DNS query to arrive on udpSocket
        // When a packet arrives, its content is placed in buffer, and information about the client (their IP and port) is stored in clientAddress.
        bytesRead = recvfrom(udpSocket, buffer, sizeof(buffer), 0, reinterpret_cast<struct sockaddr *>(&clientAddress), &clientAddrLen);
        if (bytesRead == -1){
            perror("Error receiving data");
            break;
        }

        // buffer[bytesRead] = '\0';
        std::cout << "Received " << bytesRead << " bytes: " << buffer << std::endl;

        // Parse Header
        //  Extract ID
        uint16_t req_id;
        std::memcpy(&req_id, buffer, 2);
        req_id = ntohs(req_id);

        // Extract flags
        uint16_t req_flags;
        std::memcpy(&req_flags, buffer + 2, 2);
        req_flags = ntohs(req_flags);

        // Parsing
        uint16_t opcode = (req_flags >> 11) & 0xF;
        uint16_t rd = (req_flags >> 8) & 0x1;

        // Building response flags
        uint16_t qr = 1 << 15; // QR = 1 (response)
        uint16_t aa = 0 << 10; // AA = 0
        uint16_t tc = 0 << 9;  // TC = 0
        uint16_t ra = 0 << 7;  // RA = 0
        uint16_t z = 0 << 4;   // Z = 0

        // RCODE depends on OPCODE
        uint16_t rcode = (opcode == 0) ? 0 : 4;

        // Put it together
        uint16_t resp_flags = qr | (opcode << 11) | (rd << 8) | ra | rcode;
        resp_flags = htons(resp_flags);

        //---Parse qd Count---
        uint16_t qdcount;
        std::memcpy(&qdcount, buffer + 4, 2);
        qdcount = ntohs(qdcount);

        int q_offset = 12;
        std::vector<std::vector<unsigned char>> questions;
        std::vector<uint16_t> qtypes, qclasses;

        for (int i = 0; i < qdcount; i++){
            std::vector<unsigned char> qname = parse_qname((unsigned char *)buffer, q_offset);
            questions.push_back(qname);

            uint16_t qtype;
            std::memcpy(&qtype, buffer + q_offset, 2);
            qtype = ntohs(qtype);
            q_offset += 2;
            qtypes.push_back(qtype);

            uint16_t qclass;
            std::memcpy(&qclass, buffer + q_offset, 2);
            qclass = ntohs(qclass);
            q_offset += 2;
            qclasses.push_back(qclass);
        }

        // Create an empty response
        // we need to make a dns header
        std::vector<std::vector<unsigned char>> answers;
        for (int i = 0; i < qdcount; i++){
            unsigned char fwdPacket[512];
            int fwdOffset = 0;

            // Copy header
            uint16_t id_net = htons(req_id);
            std::memcpy(fwdPacket + fwdOffset, &id_net, 2);
            fwdOffset += 2;

            uint16_t flags_net = htons(0x0100); // standard query
            std::memcpy(fwdPacket + fwdOffset, &flags_net, 2);
            fwdOffset += 2;

            uint16_t qdcount_net = htons(1); // one question at a time
            std::memcpy(fwdPacket + fwdOffset, &qdcount_net, 2);
            fwdOffset += 2;

            uint16_t zeros = 0;
            std::memcpy(fwdPacket + fwdOffset, &zeros, 2); // ANCOUNT
            fwdOffset += 2;
            std::memcpy(fwdPacket + fwdOffset, &zeros, 2); // NSCOUNT
            fwdOffset += 2;
            std::memcpy(fwdPacket + fwdOffset, &zeros, 2); // ARCOUNT
            fwdOffset += 2;

            // Copy question
            std::memcpy(fwdPacket + fwdOffset, questions[i].data(), questions[i].size());
            fwdOffset += questions[i].size();

            uint16_t qtype_net = htons(qtypes[i]);
            std::memcpy(fwdPacket + fwdOffset, &qtype_net, 2);
            fwdOffset += 2;

            uint16_t qclass_net = htons(qclasses[i]);
            std::memcpy(fwdPacket + fwdOffset, &qclass_net, 2);
            fwdOffset += 2;

            // Send to resolver
            sendto(forwardSocket, fwdPacket, fwdOffset, 0,
                   reinterpret_cast<struct sockaddr *>(&resolver), sizeof(resolver));

            // Receive response from resolver
            unsigned char fwdResp[512];
            socklen_t rlen = sizeof(resolver);
            int rbytes = recvfrom(forwardSocket, fwdResp, sizeof(fwdResp), 0,
                                  reinterpret_cast<struct sockaddr *>(&resolver), &rlen);
            if (rbytes > 0){
                // Extract answer section (after question)
                int a_offset = 12;
                // skip question in resolver response
                parse_qname(fwdResp, a_offset);
                a_offset += 4; // qtype + qclass
                // append everything else (answers)
                std::vector<unsigned char> ans(fwdResp + a_offset, fwdResp + rbytes);
                answers.push_back(ans);
            }
        }
        unsigned char response[512];
        int offset = 0;

        //---Header---
        uint16_t packet_id = htons(req_id);
        std::memcpy(response + offset, &packet_id, 2);
        offset += 2;

        // Flags: 0x8180 = std response
        std::memcpy(response + offset, &resp_flags, 2);
        offset += 2;

        // QDCOUNT
        uint16_t qdcount_net = htons(qdcount);
        std::memcpy(response + offset, &qdcount_net, 2);
        offset += 2;

        // ANCOUNT = 1
        uint16_t ancount = htons(answers.size());
        std::memcpy(response + offset, &ancount, 2);
        offset += 2;

        // NSCOUNT = 0
        uint16_t nscount = htons(0);
        std::memcpy(response + offset, &nscount, 2);
        offset += 2;

        // ARCOUNT = 0
        uint16_t arcount = htons(0);
        std::memcpy(response + offset, &arcount, 2);
        offset += 2;

        //---Question Section---

        for (int i = 0; i < qdcount; i++){
            std::memcpy(response + offset, questions[i].data(), questions[i].size());
            offset += questions[i].size();

            uint16_t qtype_net = htons(qtypes[i]);
            std::memcpy(response + offset, &qtype_net, 2);
            offset += 2;

            uint16_t qclass_net = htons(qclasses[i]);
            std::memcpy(response + offset, &qclass_net, 2);
            offset += 2;
        }

        //---Answer---
        for (auto &ans : answers){
            std::memcpy(response + offset, ans.data(), ans.size());
            offset += ans.size();
        }

        // Send response
        if (sendto(udpSocket, response, sizeof(response), 0, reinterpret_cast<struct sockaddr *>(&clientAddress), sizeof(clientAddress)) == -1){
            perror("Failed to send response");
        }
    }

    close(udpSocket);
    close(forwardSocket);

    return 0;
}

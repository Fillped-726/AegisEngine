import socket
import struct
import time
import sys

def recv_exact(sock, n):
    """辅助函数：确保读取 n 个字节"""
    data = b''
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data += packet
    return data

def read_response(sock):
    """读取服务器的回包 (Length + Payload)"""
    # 1. 读取 4 字节 Length
    header = recv_exact(sock, 4)
    if not header:
        print("Server closed connection during header read.")
        return None
    
    # 解析长度 (Big Endian)
    (total_len,) = struct.unpack('!I', header)
    
    # 2. 读取 Body
    body = recv_exact(sock, total_len)
    if not body:
        print("Server closed connection during body read.")
        return None
        
    return body

def send_packet(sock, msg_id, content):
    """发送包并等待回显"""
    # 构造 payload: MsgID (4 bytes) + Content
    payload = struct.pack('!I', msg_id) + content
    
    # 构造 header: Length (4 bytes)
    header = struct.pack('!I', len(payload))
    
    print(f"Sending MsgID={msg_id}, Content='{content.decode(errors='ignore')}'...")
    sock.sendall(header + payload)

    # --- 关键：发送后立即读取回显 ---
    print("Waiting for echo...")
    response_payload = read_response(sock)
    
    if response_payload:
        # 解析回包
        recv_msg_id_bytes = response_payload[:4]
        recv_content = response_payload[4:]
        
        (recv_msg_id,) = struct.unpack('!I', recv_msg_id_bytes)
        print(f"✅ Echo Received! MsgID={recv_msg_id}, Content='{recv_content.decode(errors='ignore')}'")
        print("-" * 30)
    else:
        print("❌ Failed to receive echo.")

def main():
    server_ip = "127.0.0.1"
    server_port = 8888

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((server_ip, server_port))
        print(f"Connected to {server_ip}:{server_port}")

        # 测试 1: 普通包
        send_packet(sock, 1001, b"Hello World")

        # 测试 2: 再次发送 (验证连接保持)
        time.sleep(0.5)
        send_packet(sock, 1002, b"Keep Alive Test")

        # 测试 3: 粘包测试 (一次性发两个包)
        print("Testing TCP Stick Package (Sending 2 packets at once)...")
        
        # 包 1
        p1_payload = struct.pack('!I', 2001) + b"PacketOne"
        p1 = struct.pack('!I', len(p1_payload)) + p1_payload
        
        # 包 2
        p2_payload = struct.pack('!I', 2002) + b"PacketTwo"
        p2 = struct.pack('!I', len(p2_payload)) + p2_payload
        
        sock.sendall(p1 + p2)
        
        # 应该收到两次回显
        print("Reading Echo 1...")
        resp1 = read_response(sock)
        if resp1: print(f"✅ Echo 1 Body Len: {len(resp1)}")
        
        print("Reading Echo 2...")
        resp2 = read_response(sock)
        if resp2: print(f"✅ Echo 2 Body Len: {len(resp2)}")

    except ConnectionRefusedError:
        print("Error: Could not connect to server. Is it running?")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        sock.close()
        print("Connection closed.")

if __name__ == "__main__":
    main()
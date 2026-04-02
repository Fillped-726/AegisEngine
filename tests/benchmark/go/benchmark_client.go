package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"sync"
	"sync/atomic"
	"time"
)

// 命令行参数
var (
	addr        = flag.String("addr", "127.0.0.1:8888", "Aegis Server 地址")
	concurrency = flag.Int("c", 1000, "并发连接数 (Goroutines)")
	payloadSize = flag.Int("s", 128, "测试包体大小(Bytes)")
	duration    = flag.Int("t", 10, "压测持续时间(秒)")
)

// 性能统计原子计数器
var (
	totalReqs  uint64
	totalBytes uint64
	activeConn int32
)

func main() {
	flag.Parse()
	log.SetOutput(os.Stdout)

	fmt.Printf("🎯 目标服务器: %s\n", *addr)
	fmt.Printf("🚀 并发连接数: %d\n", *concurrency)
	fmt.Printf("📦 报文体大小: %d Bytes\n", *payloadSize)
	fmt.Printf("⏱️  压测时间: %d 秒\n", *duration)
	fmt.Println("--------------------------------------------------")

	// 1. 构造遵循 Aegis Engine (Length-Value) 协议的网络包
	// 4字节大端序包头 + 包体
	payload := make([]byte, *payloadSize)
	for i := range payload {
		payload[i] = 'A' // 填充 dummy 数据
	}

	packetLen := uint32(len(payload))
	sendBuffer := make([]byte, 4+packetLen)
	binary.BigEndian.PutUint32(sendBuffer[:4], packetLen) // 写入 4 字节 Header
	copy(sendBuffer[4:], payload)                         // 写入 Body

	var wg sync.WaitGroup
	startSignal := make(chan struct{})

	// 2. 启动指定数量的并发 Worker
	for i := 0; i < *concurrency; i++ {
		wg.Add(1)
		go worker(sendBuffer, startSignal, &wg)
	}

	// 3. 发射起跑信号，让所有 Goroutine 同时开始猛攻 (制造并发尖刺)
	close(startSignal)
	startTime := time.Now()

	// 4. 定时器：每秒打印一次实时 QPS
	ticker := time.NewTicker(time.Second)
	go func() {
		var lastReqs uint64
		for range ticker.C {
			currentReqs := atomic.LoadUint64(&totalReqs)
			qps := currentReqs - lastReqs
			lastReqs = currentReqs
			active := atomic.LoadInt32(&activeConn)
			fmt.Printf("实时统计: 活动连接=[%d] | QPS=[%d] req/s\n", active, qps)
		}
	}()

	// 5. 等待压测时间结束
	time.Sleep(time.Duration(*duration) * time.Second)
	ticker.Stop()

	// 6. 汇总打印战报
	elapsed := time.Since(startTime).Seconds()
	finalReqs := atomic.LoadUint64(&totalReqs)
	finalBytes := atomic.LoadUint64(&totalBytes)

	avgQPS := float64(finalReqs) / elapsed
	// 吞吐量 = (总字节数 / 1024 / 1024) / 秒数 -> MB/s
	throughput := (float64(finalBytes) / 1024 / 1024) / elapsed

	fmt.Println("--------------------------------------------------")
	fmt.Println("🔥 压测结束! 战报汇总:")
	fmt.Printf("   总耗时: %.2f 秒\n", elapsed)
	fmt.Printf("   完成请求: %d 次\n", finalReqs)
	fmt.Printf("   平均 QPS: %.2f req/s\n", avgQPS)
	fmt.Printf("   网络吞吐量: %.2f MB/s\n", throughput)
	fmt.Println("--------------------------------------------------")
	os.Exit(0) // 直接退出，懒得等残余连接慢慢 close
}

// 单个连接的读写死循环
func worker(sendBuffer []byte, startSignal <-chan struct{}, wg *sync.WaitGroup) {
	defer wg.Done()

	// 阻塞等待起跑信号
	<-startSignal

	conn, err := net.Dial("tcp", *addr)
	if err != nil {
		// 并发太高时可能会耗尽端口或被拒绝，这里静默退出即可
		return
	}
	defer conn.Close()

	atomic.AddInt32(&activeConn, 1)
	defer atomic.AddInt32(&activeConn, -1)

	recvBuffer := make([]byte, len(sendBuffer))

	for {
		// 1. 发送包 (Write)
		_, err := conn.Write(sendBuffer)
		if err != nil {
			return
		}

		// 2. 接收完整的回显包 (ReadFull 保证读满指定的字节数)
		// 这对应了 Aegis Engine 中 Echo Server 原封不动发回来的行为
		_, err = io.ReadFull(conn, recvBuffer)
		if err != nil {
			return
		}

		// 3. 统计指标 (一次完整的 Ping-Pong 算作 1 次请求)
		atomic.AddUint64(&totalReqs, 1)
		// 统计吞吐量 (发送的字节 + 接收的字节)
		atomic.AddUint64(&totalBytes, uint64(len(sendBuffer)*2))
	}
}
# Cellular Network Module for STM32U5

This module provides cellular connectivity for the STM32U5 IoT Reference project using a Sierra Wireless HL7810 modem (originally started with SIM7600G).

## Hardware Configuration

### Connections
- **UART**: USART3 on Arduino connector CN13
  - CN13 D0 (PD9) - RX (connects to modem TX, Pin 8)
  - CN13 D1 (PD8) - TX (connects to modem RX, Pin 10)
  - CN14 GND - Ground (connects to modem GND, Pin 6)
- **Baud Rate**: 115200
- **Flow Control**: None
- **DMA**: Channel 6 for efficient RX operations

### Modem
- **Current**: Sierra Wireless HL7810 (LTE Cat-M1/NB-IoT)
- **Previous**: SIM7600G (4G LTE)
- **SIM**: Monogoto IoT SIM
- **APN**: `data.mono`

## Architecture

### Module Structure

```
cellular/
├── cellular_netconn.h      - Public API for cellular network
├── cellular_netconn.c      - Main state machine and connection manager
├── cellular_prv.h          - Private definitions and structures
├── cellular_at.c           - AT command interface
├── cellular_ppp.c          - PPP/lwIP integration
├── cellular_uart.c         - UART driver (DMA-based)
└── cellular_uart_irq.c     - USART3 interrupt handler
```

### Components

#### 1. UART Driver ([cellular_uart.c](cellular_uart.c))
- DMA-based circular buffer reception
- Stream buffer interface for both AT and PPP modes
- ISR-driven with dedicated RX task
- Mode switching between AT command and PPP data mode
- STM32U5-specific optimizations (FIFO mode, error handling)

#### 2. AT Command Interface ([cellular_at.c](cellular_at.c))
- Sierra Wireless HL7810 AT command set
- Modem initialization and configuration
- SIM card detection and status checking
- Network registration with timeout handling
- APN configuration
- PPP session start/stop
- Critical buffer flushing to prevent PPP corruption

#### 3. PPP Integration ([cellular_ppp.c](cellular_ppp.c))
- lwIP PPPoS (PPP over Serial) implementation
- Bridge task: UART RX → lwIP PPP stack
- Output callback: lwIP PPP → UART TX
- Link status monitoring and event notifications
- DNS server handling via IPCP

#### 4. Connection Manager ([cellular_netconn.c](cellular_netconn.c))
- State machine for connection lifecycle
- Event-driven architecture with FreeRTOS notifications
- Automatic reconnection on failure
- Integration with system event framework
- Status reporting for application layer

## Current State: PPP Mode Implementation

### What Works ✓

1. **Hardware Communication**
   - UART3 configured with DMA for efficient data transfer
   - Reliable bidirectional communication with modem
   - Proper interrupt handling and error recovery

2. **AT Command Interface**
   - Modem initialization and radio activation (AT+CFUN=1)
   - Automatic network selection (AT+COPS=0)
   - SIM card detection and validation
   - Network registration (LTE/NB-IoT via AT+CEREG)
   - Signal strength reporting
   - APN configuration for Monogoto SIM

3. **PPP Connection**
   - UART spam issue **RESOLVED** (see fixes below)
   - PPP negotiation completes successfully
   - IP address assignment via IPCP
   - Gateway and netmask configuration
   - lwIP integration with default route

4. **Critical Fixes Applied**
   - Disabled echo **before** any AT commands to prevent buffer pollution
   - Added comprehensive buffer flushing:
     - Before modem initialization (stale data from retries)
     - After boot (modem URCs like RDY, +CPIN)
     - Before PPP transition (pending AT responses)
   - Disabled registration URCs (AT+CEREG=0) before PPP to prevent stream corruption
   - Proper mode switching between AT and PPP
   - FIFO mode enabled for STM32U5 DMA reliability

### Current Issues ✗

#### DNS Resolution Failure
**Status**: PPP connection established, but DNS queries fail

**Symptoms**:
- PPP negotiation completes successfully
- IP address, gateway, netmask assigned correctly
- Modem responds to DNS queries
- **lwIP is not parsing DNS responses correctly**

**Investigation Notes** (from commit d93d37d):
> "UART spam stopped, The modem is responding to DNS queries, but lwIP is not parsing it correctly."

**DNS Configuration**:
- DNS servers provided via IPCP during PPP negotiation
- Current workaround: Override DNS to use modem gateway IP
- Modem gateway issue: `/32` netmask instead of proper subnet mask

**Impact**:
- MQTT cannot resolve AWS IoT endpoint hostnames
- Application cannot connect to cloud services
- Blocks end-to-end IoT functionality

## Next Steps: Migration to AT Sockets Mode

### Why AT Sockets?

PPP mode has proven problematic with complex state management, URC pollution, and DNS issues. **AT Sockets mode** is the recommended path forward:

#### Advantages
1. **Cleaner architecture**: Commands and data clearly separated
2. **No URC pollution**: URCs are expected and parseable in AT mode
3. **Built-in TCP/IP stack**: Modem handles networking internally
4. **Better error handling**: AT responses provide clear error codes
5. **Simpler DNS**: Modem's TCP/IP stack handles DNS resolution
6. **Proven approach**: Used successfully in many cellular IoT devices

#### Disadvantages
1. **Adapter layer required**: Need to bridge AT sockets to lwIP/MQTT
2. **Vendor-specific**: AT socket commands vary between modems
3. **Limited control**: Rely on modem's TCP/IP implementation

### Implementation Plan

#### Phase 1: AT Socket Infrastructure
- [ ] Implement Sierra Wireless AT socket commands:
  - `AT+KTCPCFG` - Configure TCP socket
  - `AT+KTCPSTART` - Start TCP connection
  - `AT+KTCPSND` - Send data
  - `AT+KTCPRCV` - Receive data
  - `AT+KTCPCLOSE` - Close connection
- [ ] Create socket management layer
- [ ] Implement URC handlers for socket events
- [ ] Add data buffer management for send/receive

#### Phase 2: Transport Adapter
- [ ] Design adapter interface compatible with coreMQTT
- [ ] Implement transport send function (maps to `AT+KTCPSND`)
- [ ] Implement transport receive function (maps to `AT+KTCPRCV`)
- [ ] Add connection state management
- [ ] Handle socket errors and reconnection

#### Phase 3: TLS Integration
- [ ] Investigate modem TLS support (`AT+KTCPCFG` TLS parameters)
- [ ] Option A: Use modem's built-in TLS (if supported)
- [ ] Option B: Implement software TLS over AT socket (mbedTLS)
- [ ] Configure AWS IoT certificates

#### Phase 4: MQTT Integration
- [ ] Modify coreMQTT to use AT socket transport
- [ ] Test MQTT connect/disconnect
- [ ] Verify MQTT publish/subscribe
- [ ] Test reconnection scenarios

#### Phase 5: OTA Support
- [ ] Adapt OTA agent to use AT sockets
- [ ] Test firmware download over cellular
- [ ] Verify OTA update process

### Alternative: Hybrid Approach
Keep AT mode for control plane (registration, status) and PPP for data plane:
- Dynamically switch between AT and PPP modes
- Use AT commands for initialization and monitoring
- Use PPP only for active data sessions
- Requires robust mode switching implementation

### Code Changes Required

#### New Files
- `cellular_socket.c` - AT socket implementation
- `cellular_transport.c` - coreMQTT transport adapter

#### Modified Files
- `cellular_at.c` - Add socket AT commands
- `cellular_netconn.c` - New state machine for socket mode
- `app_main.c` - Update MQTT initialization
- OTA components - Update transport layer

#### Deprecated (in socket mode)
- `cellular_ppp.c` - PPP implementation (keep for reference)
- PPP-related lwIP configuration


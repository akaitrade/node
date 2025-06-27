# Credits Node WebSocket API Documentation

## Overview

The Credits Node WebSocket API provides real-time access to blockchain data through a WebSocket connection. It supports both request-response operations and event subscriptions for live blockchain monitoring.

## Configuration

### Enable WebSocket API

Add the following to your `config.ini` file in the `[api]` section:

```ini
[api]
# WebSocket API port (disabled by default, set to non-zero to enable)
websocket_port=9095
```

### Configuration Options

- **`websocket_port`**: Port number for WebSocket server
  - Default: `0` (disabled)
  - Port Range: 1-65535
  - Recommended: 9095

### SSL/TLS Support (WSS)

The WebSocket server runs in plain HTTP mode internally. For SSL/TLS support (WSS), use a reverse proxy:

#### Architecture for HTTPS/WSS Support
```
[Internet] --HTTPS/WSS--> [Reverse Proxy] --HTTP/WS--> [Credits Node]
    ↓                           ↓                         ↓
wss://domain.com:443      SSL Termination          ws://localhost:9095
```

This approach provides:
- **Better security** - SSL handled by dedicated proxy
- **Automatic certificate management** - Let's Encrypt integration
- **Better performance** - Optimized SSL termination
- **Additional features** - Rate limiting, caching, load balancing

### Build Requirements

The WebSocket API requires the following compile-time definitions:
- `WEBSOCKET_API` - Enable WebSocket functionality
- Uses Boost.Asio for networking
- Requires nlohmann/json for JSON serialization

## Connection

### WebSocket Endpoints

#### Direct Connection (Development/Local)
```
ws://your-node-ip:9095/   # Direct connection to Credits Node
```

#### Production Connection (via Reverse Proxy)
```
wss://your-domain.com/    # HTTPS/WSS via reverse proxy
```

### Reverse Proxy Setup for Production

For production deployments with HTTPS/WSS support, configure a reverse proxy:

#### Nginx Configuration
```nginx
server {
    listen 443 ssl http2;
    server_name server.livelegends.com;
    
    # SSL Configuration
    ssl_certificate /etc/letsencrypt/live/server.livelegends.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/server.livelegends.com/privkey.pem;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_ciphers ECDHE-RSA-AES256-GCM-SHA512:DHE-RSA-AES256-GCM-SHA512:ECDHE-RSA-AES256-GCM-SHA384:DHE-RSA-AES256-GCM-SHA384;
    
    # WebSocket proxying
    location / {
        proxy_pass http://localhost:9095;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        
        # Optional: Rate limiting
        limit_req zone=api burst=20 nodelay;
    }
}

# Rate limiting zone (add to http block)
limit_req_zone $binary_remote_addr zone=api:10m rate=10r/s;
```

#### Caddy Configuration (Automatic HTTPS)
```caddy
server.livelegends.com {
    reverse_proxy localhost:9095
    
    # Optional: Rate limiting
    rate_limit {
        zone static 10r/s
    }
}
```

#### Apache Configuration
```apache
<VirtualHost *:443>
    ServerName server.livelegends.com
    
    # SSL Configuration
    SSLEngine on
    SSLCertificateFile /etc/letsencrypt/live/server.livelegends.com/cert.pem
    SSLCertificateKeyFile /etc/letsencrypt/live/server.livelegends.com/privkey.pem
    SSLCertificateChainFile /etc/letsencrypt/live/server.livelegends.com/chain.pem
    
    # WebSocket proxying
    ProxyPreserveHost On
    ProxyPass / ws://localhost:9095/
    ProxyPassReverse / ws://localhost:9095/
    
    # Enable WebSocket support
    RewriteEngine On
    RewriteCond %{HTTP:Upgrade} websocket [NC]
    RewriteCond %{HTTP:Connection} upgrade [NC]
    RewriteRule ^/?(.*) ws://localhost:9095/$1 [P,L]
</VirtualHost>
```

### Automatic SSL Certificate Setup

#### Let's Encrypt with Certbot (Nginx)
```bash
# Install certbot
sudo apt update
sudo apt install certbot python3-certbot-nginx

# Get certificate and configure nginx
sudo certbot --nginx -d server.livelegends.com

# Verify auto-renewal
sudo certbot renew --dry-run
```

#### Let's Encrypt with Certbot (Apache)
```bash
# Install certbot
sudo apt update
sudo apt install certbot python3-certbot-apache

# Get certificate and configure apache
sudo certbot --apache -d server.livelegends.com
```

#### Caddy (Automatic)
Caddy automatically obtains and renews Let's Encrypt certificates with zero configuration!

### Message Format

All messages use JSON format with the following structure:

```json
{
  "type": <message_type_number>,
  "id": "<unique_message_id>",
  "data": {
    // Message-specific data
  }
}
```

## Message Types

### Request Types (1-99)
- `1` - GetStatus
- `2` - GetBalance
- `3` - GetTransaction
- `4` - GetPool
- `5` - GetPools
- `6` - GetPoolsInfo
- `7` - GetTransactions
- `8` - GetLastBlockInfo
- `9` - GetCounters
- `10` - GetSmartContract
- `11` - GetSmartContracts
- `12` - GetSmartContractAddresses

### Subscription Types (100-199)
- `100` - Subscribe
- `101` - Unsubscribe

### Notification Types (200-299)
- `200` - NewBlock
- `201` - NewTransaction
- `202` - TransactionStatus
- `203` - SmartContractEvent

### System Types (400-599)
- `400` - Error
- `500` - Ping
- `501` - Pong

## API Functions

### 1. GetStatus (Type: 1)

Get the current synchronization status of the node.

**Request:**
```json
{
  "type": 1,
  "id": "status-1",
  "data": {}
}
```

**Response:**
```json
{
  "type": 1,
  "id": "status-1",
  "data": {
    "currRound": 12345,
    "lastBlock": 67890
  }
}
```

### 2. GetBalance (Type: 2)

Get the balance of a specific address.

**Request:**
```json
{
  "type": 2,
  "id": "balance-1",
  "data": {
    "address": "base64_encoded_address"
  }
}
```

**Response:**
```json
{
  "type": 2,
  "id": "balance-1",
  "data": {
    "address": "base64_encoded_address",
    "balance": 123.456789,
    "integral": 123,
    "fraction": 456789000000000000
  }
}
```

### 3. GetTransaction (Type: 3)

Get details of a specific transaction.

**Request:**
```json
{
  "type": 3,
  "id": "tx-1",
  "data": {
    "poolSeq": 1234,
    "index": 5
  }
}
```

**Response:**
```json
{
  "type": 3,
  "id": "tx-1",
  "data": {
    "found": true,
    "poolSeq": 1234,
    "index": 5,
    "source": "base64_encoded_source_address",
    "target": "base64_encoded_target_address",
    "amount": 10.5,
    "currency": 1
  }
}
```

### 4. GetPool (Type: 4)

Get information about a specific pool/block.

**Request:**
```json
{
  "type": 4,
  "id": "pool-1",
  "data": {
    "sequence": 1234
  }
}
```

**Response:**
```json
{
  "type": 4,
  "id": "pool-1",
  "data": {
    "sequence": 1234,
    "hash": "base64_encoded_hash",
    "prevHash": "base64_encoded_prev_hash",
    "time": 1634567890123,
    "transactionsCount": 25
  }
}
```

### 5. GetPools (Type: 5)

Get a list of pools with pagination.

**Request:**
```json
{
  "type": 5,
  "id": "pools-1",
  "data": {
    "offset": 0,
    "limit": 10
  }
}
```

**Response:**
```json
{
  "type": 5,
  "id": "pools-1",
  "data": {
    "pools": [
      {
        "sequence": 1234,
        "hash": "base64_encoded_hash",
        "time": 1634567890123,
        "transactionsCount": 25
      }
    ]
  }
}
```

### 6. GetPoolsInfo (Type: 6)

Get detailed information about pools with pagination.

**Request:**
```json
{
  "type": 6,
  "id": "pools-info-1",
  "data": {
    "offset": 0,
    "limit": 10
  }
}
```

**Response:**
```json
{
  "type": 6,
  "id": "pools-info-1",
  "data": {
    "pools": [
      {
        "sequence": 1234,
        "hash": "base64_encoded_hash",
        "prevHash": "base64_encoded_prev_hash",
        "time": 1634567890123,
        "transactionsCount": 25
      }
    ]
  }
}
```

### 7. GetTransactions (Type: 7)

Get transactions for a specific address with pagination.

**Request:**
```json
{
  "type": 7,
  "id": "txs-1",
  "data": {
    "address": "base64_encoded_address",
    "offset": 0,
    "limit": 10
  }
}
```

**Response:**
```json
{
  "type": 7,
  "id": "txs-1",
  "data": {
    "transactions": [
      {
        "poolSeq": 1234,
        "index": 5,
        "source": "base64_encoded_source",
        "target": "base64_encoded_target",
        "amount": 10.5,
        "currency": 1
      }
    ]
  }
}
```

### 8. GetLastBlockInfo (Type: 8)

Get information about the latest block.

**Request:**
```json
{
  "type": 8,
  "id": "last-block-1",
  "data": {}
}
```

**Response:**
```json
{
  "type": 8,
  "id": "last-block-1",
  "data": {
    "sequence": 12345,
    "hash": "base64_encoded_hash",
    "prevHash": "base64_encoded_prev_hash",
    "time": 1634567890123,
    "transactionsCount": 42
  }
}
```

### 9. GetCounters (Type: 9)

Get network statistics and counters.

**Request:**
```json
{
  "type": 9,
  "id": "stats-1",
  "data": {}
}
```

**Response:**
```json
{
  "type": 9,
  "id": "stats-1",
  "data": {
    "stats": [
      {
        "periodDuration": 300000,
        "poolsCount": 50,
        "transactionsCount": 1250,
        "smartContractsCount": 15
      }
    ]
  }
}
```

### 10. GetSmartContract (Type: 10)

Get details of a specific smart contract.

**Request:**
```json
{
  "type": 10,
  "id": "sc-1",
  "data": {
    "address": "base64_encoded_contract_address"
  }
}
```

**Response:**
```json
{
  "type": 10,
  "id": "sc-1",
  "data": {
    "address": "base64_encoded_contract_address",
    "deployer": "base64_encoded_deployer_address",
    "objectState": "base64_encoded_state"
  }
}
```

### 11. GetSmartContracts (Type: 11)

Get smart contracts deployed by a specific address.

**Request:**
```json
{
  "type": 11,
  "id": "scs-1",
  "data": {
    "deployer": "base64_encoded_deployer_address",
    "offset": 0,
    "limit": 10
  }
}
```

**Response:**
```json
{
  "type": 11,
  "id": "scs-1",
  "data": {
    "smartContracts": [
      {
        "address": "base64_encoded_contract_address",
        "deployer": "base64_encoded_deployer_address",
        "objectState": "base64_encoded_state"
      }
    ]
  }
}
```

### 12. GetSmartContractAddresses (Type: 12)

Get a list of smart contract addresses deployed by a specific address.

**Request:**
```json
{
  "type": 12,
  "id": "sc-addrs-1",
  "data": {
    "deployer": "base64_encoded_deployer_address"
  }
}
```

**Response:**
```json
{
  "type": 12,
  "id": "sc-addrs-1",
  "data": {
    "addresses": [
      "base64_encoded_contract_address_1",
      "base64_encoded_contract_address_2"
    ]
  }
}
```

## Subscriptions

### Subscribe (Type: 100)

Subscribe to real-time events.

**Available Topics:**
- `"blocks"` - New block notifications
- `"transactions"` - New transaction notifications
- `"smart_contracts"` - Smart contract events
- `"tx:TRANSACTION_ID"` - Specific transaction status updates

**Request:**
```json
{
  "type": 100,
  "id": "sub-1",
  "data": {
    "topic": "blocks"
  }
}
```

**Response:**
```json
{
  "type": 100,
  "id": "sub-1",
  "data": {
    "subscribed": "blocks"
  }
}
```

### Unsubscribe (Type: 101)

Unsubscribe from events.

**Request:**
```json
{
  "type": 101,
  "id": "unsub-1",
  "data": {
    "topic": "blocks"
  }
}
```

**Response:**
```json
{
  "type": 101,
  "id": "unsub-1",
  "data": {
    "unsubscribed": "blocks"
  }
}
```

## Event Notifications

**Status: ✅ IMPLEMENTED** - Real-time notifications are now fully functional and connected to blockchain events.

### NewBlock (Type: 200)

Sent when a new block is created (requires "blocks" subscription). Triggered automatically when the node processes new blocks.

```json
{
  "type": 200,
  "id": "",
  "data": {
    "sequence": 12346,
    "hash": "block_hash_string",
    "prevHash": "previous_block_hash_string", 
    "time": 1634567890456,
    "transactionsCount": 18
  }
}
```

### NewTransaction (Type: 201)

Sent when a new transaction is detected (requires "transactions" subscription). Triggered automatically for each transaction in newly processed blocks.

```json
{
  "type": 201,
  "id": "",
  "data": {
    "poolSeq": 12346,
    "index": 7,
    "time": 1634567890456
  }
}
```

**Note**: Use `GetTransaction` API with the provided `poolSeq` and `index` to retrieve full transaction details including Base58-encoded addresses.

### TransactionStatus (Type: 202)

Sent when a subscribed transaction status changes.

```json
{
  "type": 202,
  "id": "",
  "data": {
    "transactionId": "tx_id",
    "status": {
      "confirmed": true,
      "block": 12346
    }
  }
}
```

### SmartContractEvent (Type: 203)

Sent when smart contract events occur (requires "smart_contracts" subscription).

```json
{
  "type": 203,
  "id": "",
  "data": {
    "contractAddress": "base64_encoded_contract_address",
    "event": "ContractExecuted",
    "data": {}
  }
}
```

## Error Handling

### Error Response (Type: 400)

```json
{
  "type": 400,
  "id": "original-message-id",
  "data": {
    "error": "Error description"
  }
}
```

**Common Error Messages:**
- `"Address not found"`
- `"Pool not found"`
- `"Smart contract not found"`
- `"Last block not found"`
- `"Unknown message type"`
- `"Error getting [resource]: [detailed error]"`

## Examples

### JavaScript Client Example

```javascript
const ws = new WebSocket('ws://localhost:9095/');

ws.onopen = function() {
    console.log('Connected to Credits Node WebSocket API');
    
    // Get node status
    ws.send(JSON.stringify({
        type: 1,
        id: 'status-request',
        data: {}
    }));
    
    // Subscribe to new blocks
    ws.send(JSON.stringify({
        type: 100,
        id: 'block-subscription',
        data: { topic: 'blocks' }
    }));
};

ws.onmessage = function(event) {
    const message = JSON.parse(event.data);
    console.log('Received:', message);
    
    switch(message.type) {
        case 1: // GetStatus response
            console.log('Node status:', message.data);
            break;
        case 100: // Subscribe response
            console.log('Subscribed to:', message.data.subscribed);
            break;
        case 200: // NewBlock notification
            console.log('New block:', message.data);
            break;
        case 400: // Error
            console.error('Error:', message.data.error);
            break;
    }
};

ws.onerror = function(error) {
    console.error('WebSocket error:', error);
};
```

### Python Client Example

```python
import asyncio
import websockets
import json

async def client():
    uri = "ws://localhost:9095/"
    
    async with websockets.connect(uri) as websocket:
        # Get balance
        balance_request = {
            "type": 2,
            "id": "balance-1",
            "data": {
                "address": "your_base64_encoded_address"
            }
        }
        
        await websocket.send(json.dumps(balance_request))
        
        # Subscribe to transactions
        subscribe_request = {
            "type": 100,
            "id": "sub-1",
            "data": {
                "topic": "transactions"
            }
        }
        
        await websocket.send(json.dumps(subscribe_request))
        
        # Listen for messages
        async for message in websocket:
            data = json.loads(message)
            print(f"Received: {data}")

# Run the client
asyncio.run(client())
```

### HTML/JavaScript Web Client Example

```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Credits Node WebSocket API Client</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            max-width: 1200px;
            margin: 0 auto;
            padding: 20px;
            background-color: #f5f5f5;
        }
        .container {
            background: white;
            border-radius: 8px;
            padding: 20px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
            margin-bottom: 20px;
        }
        .controls {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
            gap: 15px;
            margin-bottom: 20px;
        }
        .control-group {
            display: flex;
            flex-direction: column;
        }
        label {
            font-weight: bold;
            margin-bottom: 5px;
            color: #333;
        }
        input, select, button {
            padding: 8px;
            border: 1px solid #ddd;
            border-radius: 4px;
            font-size: 14px;
        }
        button {
            background-color: #007bff;
            color: white;
            border: none;
            cursor: pointer;
            font-weight: bold;
            margin-top: 5px;
        }
        button:hover {
            background-color: #0056b3;
        }
        button:disabled {
            background-color: #ccc;
            cursor: not-allowed;
        }
        .status {
            padding: 10px;
            border-radius: 4px;
            margin-bottom: 15px;
            font-weight: bold;
        }
        .status.connected {
            background-color: #d4edda;
            color: #155724;
            border: 1px solid #c3e6cb;
        }
        .status.disconnected {
            background-color: #f8d7da;
            color: #721c24;
            border: 1px solid #f5c6cb;
        }
        .status.connecting {
            background-color: #fff3cd;
            color: #856404;
            border: 1px solid #ffeaa7;
        }
        .log {
            background-color: #f8f9fa;
            border: 1px solid #dee2e6;
            border-radius: 4px;
            padding: 15px;
            height: 400px;
            overflow-y: auto;
            font-family: 'Courier New', monospace;
            font-size: 12px;
            white-space: pre-wrap;
        }
        .subscription-controls {
            display: flex;
            gap: 10px;
            align-items: center;
            flex-wrap: wrap;
        }
        .response-data {
            background-color: #e9ecef;
            border-radius: 4px;
            padding: 10px;
            margin-top: 10px;
            font-family: 'Courier New', monospace;
            font-size: 12px;
            max-height: 200px;
            overflow-y: auto;
        }
    </style>
</head>
<body>
    <h1>Credits Node WebSocket API Client</h1>
    
    <div class="container">
        <h2>Connection</h2>
        <div class="controls">
            <div class="control-group">
                <label for="wsUrl">WebSocket URL:</label>
                <input type="text" id="wsUrl" value="ws://localhost:9095/" placeholder="ws://localhost:9095/">
            </div>
            <div class="control-group">
                <button id="connectBtn" onclick="connect()">Connect</button>
                <button id="disconnectBtn" onclick="disconnect()" disabled>Disconnect</button>
            </div>
        </div>
        <div id="connectionStatus" class="status disconnected">Disconnected</div>
    </div>

    <div class="container">
        <h2>API Requests</h2>
        <div class="controls">
            <div class="control-group">
                <label for="requestType">Request Type:</label>
                <select id="requestType" onchange="updateRequestForm()">
                    <option value="1">GetStatus</option>
                    <option value="2">GetBalance</option>
                    <option value="3">GetTransaction</option>
                    <option value="4">GetPool</option>
                    <option value="5">GetPools</option>
                    <option value="6">GetPoolsInfo</option>
                    <option value="7">GetTransactions</option>
                    <option value="8">GetLastBlockInfo</option>
                    <option value="9">GetCounters</option>
                    <option value="10">GetSmartContract</option>
                    <option value="11">GetSmartContracts</option>
                    <option value="12">GetSmartContractAddresses</option>
                </select>
            </div>
            <div class="control-group">
                <button onclick="sendRequest()" id="sendRequestBtn" disabled>Send Request</button>
            </div>
        </div>
        
        <!-- Dynamic form fields will be inserted here -->
        <div id="requestForm" class="controls"></div>
        
        <div id="lastResponse" class="response-data" style="display:none;"></div>
    </div>

    <div class="container">
        <h2>Subscriptions</h2>
        <div class="subscription-controls">
            <select id="subscriptionTopic">
                <option value="blocks">Blocks</option>
                <option value="transactions">Transactions</option>
                <option value="smart_contracts">Smart Contracts</option>
            </select>
            <button onclick="subscribe()" id="subscribeBtn" disabled>Subscribe</button>
            <button onclick="unsubscribe()" id="unsubscribeBtn" disabled>Unsubscribe</button>
        </div>
        <div id="subscriptionStatus"></div>
    </div>

    <div class="container">
        <h2>Message Log</h2>
        <button onclick="clearLog()" style="margin-bottom: 10px;">Clear Log</button>
        <div id="messageLog" class="log"></div>
    </div>

    <script>
        let ws = null;
        let messageCounter = 0;
        let activeSubscriptions = new Set();

        const requestForms = {
            1: [], // GetStatus - no parameters
            2: [{ name: 'address', label: 'Address (Base64)', type: 'text', placeholder: 'Base64 encoded address' }],
            3: [
                { name: 'poolSeq', label: 'Pool Sequence', type: 'number', placeholder: '1234' },
                { name: 'index', label: 'Transaction Index', type: 'number', placeholder: '0' }
            ],
            4: [{ name: 'sequence', label: 'Pool Sequence', type: 'number', placeholder: '1234' }],
            5: [
                { name: 'offset', label: 'Offset', type: 'number', placeholder: '0', value: '0' },
                { name: 'limit', label: 'Limit', type: 'number', placeholder: '10', value: '10' }
            ],
            6: [
                { name: 'offset', label: 'Offset', type: 'number', placeholder: '0', value: '0' },
                { name: 'limit', label: 'Limit', type: 'number', placeholder: '10', value: '10' }
            ],
            7: [
                { name: 'address', label: 'Address (Base64)', type: 'text', placeholder: 'Base64 encoded address' },
                { name: 'offset', label: 'Offset', type: 'number', placeholder: '0', value: '0' },
                { name: 'limit', label: 'Limit', type: 'number', placeholder: '10', value: '10' }
            ],
            8: [], // GetLastBlockInfo - no parameters
            9: [], // GetCounters - no parameters
            10: [{ name: 'address', label: 'Contract Address (Base64)', type: 'text', placeholder: 'Base64 encoded contract address' }],
            11: [
                { name: 'deployer', label: 'Deployer Address (Base64)', type: 'text', placeholder: 'Base64 encoded deployer address' },
                { name: 'offset', label: 'Offset', type: 'number', placeholder: '0', value: '0' },
                { name: 'limit', label: 'Limit', type: 'number', placeholder: '10', value: '10' }
            ],
            12: [{ name: 'deployer', label: 'Deployer Address (Base64)', type: 'text', placeholder: 'Base64 encoded deployer address' }]
        };

        function connect() {
            const url = document.getElementById('wsUrl').value;
            
            updateConnectionStatus('connecting', 'Connecting...');
            logMessage('Attempting to connect to: ' + url);
            
            try {
                ws = new WebSocket(url);
                
                ws.onopen = function() {
                    updateConnectionStatus('connected', 'Connected');
                    logMessage('✅ Connected to Credits Node WebSocket API');
                    updateButtonStates(true);
                };
                
                ws.onmessage = function(event) {
                    const message = JSON.parse(event.data);
                    logMessage('📨 Received: ' + JSON.stringify(message, null, 2));
                    
                    // Show response data
                    if (message.type < 100) {
                        showResponseData(message);
                    }
                    
                    // Handle subscription confirmations
                    if (message.type === 100) {
                        activeSubscriptions.add(message.data.subscribed);
                        updateSubscriptionStatus();
                    } else if (message.type === 101) {
                        activeSubscriptions.delete(message.data.unsubscribed);
                        updateSubscriptionStatus();
                    }
                };
                
                ws.onerror = function(error) {
                    console.error('WebSocket error details:', error);
                    logMessage('❌ WebSocket error: ' + error.type + ' - Check server status and network connectivity');
                    updateConnectionStatus('disconnected', 'Error occurred');
                };
                
                ws.onclose = function() {
                    updateConnectionStatus('disconnected', 'Disconnected');
                    logMessage('🔌 Connection closed');
                    updateButtonStates(false);
                    activeSubscriptions.clear();
                    updateSubscriptionStatus();
                };
                
            } catch (error) {
                logMessage('❌ Connection failed: ' + error.message);
                updateConnectionStatus('disconnected', 'Connection failed');
            }
        }

        function disconnect() {
            if (ws) {
                ws.close();
                ws = null;
            }
        }

        function sendRequest() {
            if (!ws || ws.readyState !== WebSocket.OPEN) {
                logMessage('❌ Not connected');
                return;
            }

            const requestType = parseInt(document.getElementById('requestType').value);
            const messageId = 'req-' + (++messageCounter);
            
            const data = {};
            const formFields = requestForms[requestType];
            
            for (const field of formFields) {
                const element = document.getElementById('field-' + field.name);
                if (element) {
                    let value = element.value;
                    if (field.type === 'number') {
                        value = parseInt(value);
                    }
                    data[field.name] = value;
                }
            }

            const message = {
                type: requestType,
                id: messageId,
                data: data
            };

            ws.send(JSON.stringify(message));
            logMessage('📤 Sent: ' + JSON.stringify(message, null, 2));
        }

        function subscribe() {
            if (!ws || ws.readyState !== WebSocket.OPEN) {
                logMessage('❌ Not connected');
                return;
            }

            const topic = document.getElementById('subscriptionTopic').value;
            const messageId = 'sub-' + (++messageCounter);

            const message = {
                type: 100,
                id: messageId,
                data: { topic: topic }
            };

            ws.send(JSON.stringify(message));
            logMessage('📤 Subscribe: ' + JSON.stringify(message, null, 2));
        }

        function unsubscribe() {
            if (!ws || ws.readyState !== WebSocket.OPEN) {
                logMessage('❌ Not connected');
                return;
            }

            const topic = document.getElementById('subscriptionTopic').value;
            const messageId = 'unsub-' + (++messageCounter);

            const message = {
                type: 101,
                id: messageId,
                data: { topic: topic }
            };

            ws.send(JSON.stringify(message));
            logMessage('📤 Unsubscribe: ' + JSON.stringify(message, null, 2));
        }

        function updateRequestForm() {
            const requestType = parseInt(document.getElementById('requestType').value);
            const formContainer = document.getElementById('requestForm');
            const fields = requestForms[requestType];
            
            formContainer.innerHTML = '';
            
            for (const field of fields) {
                const div = document.createElement('div');
                div.className = 'control-group';
                
                const label = document.createElement('label');
                label.textContent = field.label + ':';
                label.setAttribute('for', 'field-' + field.name);
                
                const input = document.createElement('input');
                input.type = field.type;
                input.id = 'field-' + field.name;
                input.placeholder = field.placeholder;
                if (field.value) {
                    input.value = field.value;
                }
                
                div.appendChild(label);
                div.appendChild(input);
                formContainer.appendChild(div);
            }
        }

        function updateConnectionStatus(status, text) {
            const statusDiv = document.getElementById('connectionStatus');
            statusDiv.className = 'status ' + status;
            statusDiv.textContent = text;
        }

        function updateButtonStates(connected) {
            document.getElementById('connectBtn').disabled = connected;
            document.getElementById('disconnectBtn').disabled = !connected;
            document.getElementById('sendRequestBtn').disabled = !connected;
            document.getElementById('subscribeBtn').disabled = !connected;
            document.getElementById('unsubscribeBtn').disabled = !connected;
        }

        function updateSubscriptionStatus() {
            const statusDiv = document.getElementById('subscriptionStatus');
            if (activeSubscriptions.size > 0) {
                statusDiv.innerHTML = '<strong>Active subscriptions:</strong> ' + Array.from(activeSubscriptions).join(', ');
                statusDiv.style.color = '#155724';
                statusDiv.style.backgroundColor = '#d4edda';
                statusDiv.style.padding = '8px';
                statusDiv.style.borderRadius = '4px';
                statusDiv.style.marginTop = '10px';
            } else {
                statusDiv.innerHTML = '';
                statusDiv.style.backgroundColor = 'transparent';
                statusDiv.style.padding = '0';
            }
        }

        function showResponseData(message) {
            const responseDiv = document.getElementById('lastResponse');
            responseDiv.style.display = 'block';
            responseDiv.innerHTML = '<strong>Last Response:</strong><br>' + JSON.stringify(message.data, null, 2);
        }

        function logMessage(message) {
            const logDiv = document.getElementById('messageLog');
            const timestamp = new Date().toLocaleTimeString();
            logDiv.textContent += `[${timestamp}] ${message}\n`;
            logDiv.scrollTop = logDiv.scrollHeight;
        }

        function clearLog() {
            document.getElementById('messageLog').textContent = '';
        }

        // Initialize form
        updateRequestForm();
        updateButtonStates(false);
    </script>
</body>
</html>
```

## Implementation Details

### Connection Management
- Each WebSocket connection is managed independently
- Connections automatically clean up subscriptions on disconnect
- The server supports multiple concurrent connections

### Threading
- The WebSocket server runs in its own thread
- Message handling is thread-safe using mutexes for subscription management
- Event notifications are broadcast to all relevant subscribers

### JSON Serialization
- Uses nlohmann/json library for JSON processing
- Binary data (addresses, hashes) are base64 encoded
- Numeric values maintain precision using integral/fraction parts for amounts

### Performance Considerations
- Subscription filtering is done per connection
- Event notifications are only sent to subscribers of relevant topics
- Large responses are paginated using offset/limit parameters

## Security Notes

### General Security
- The WebSocket server does not implement authentication
- **Always use a reverse proxy in production** for SSL termination
- Limit access to trusted networks only
- Monitor for excessive connection attempts

### Production Security Best Practices

#### 1. Use Reverse Proxy
- **Never expose** the WebSocket server directly to the internet
- Use nginx, Caddy, or Apache for SSL termination
- Configure proper security headers

#### 2. Network Security
```bash
# Allow only local connections to WebSocket server
sudo ufw allow from 127.0.0.1 to any port 9095
sudo ufw deny 9095

# Allow HTTPS/WSS through reverse proxy
sudo ufw allow 443
```

#### 3. Rate Limiting
Configure rate limiting in your reverse proxy:
- **Nginx**: `limit_req_zone` and `limit_req`
- **Caddy**: `rate_limit` directive
- **Apache**: `mod_evasive` or `mod_qos`

#### 4. Access Control
- Use firewall rules to restrict access
- Consider VPN for administrative access
- Implement IP whitelisting if needed

#### 5. Monitoring
- Monitor connection counts and patterns
- Set up alerts for unusual activity
- Log all WebSocket connections

## Troubleshooting

### Common Issues

1. **Connection Refused**
   - Check if `websocket_port` is set to non-zero in config.ini
   - Verify the port is not blocked by firewall
   - Ensure the node is running and the API is enabled

2. **No Response to Messages**
   - Verify message format matches the specification
   - Check that `type` field contains valid message type number
   - Ensure `id` field is present and unique

3. **Subscription Not Working**
   - Confirm successful subscription response before expecting notifications
   - Check that events are actually occurring on the blockchain
   - Verify connection remains open
   - **NEW**: Real-time subscriptions are now fully implemented and working

4. **Address Format Issues (FIXED)**
   - **Old Issue**: API returned addresses with null bytes (`\u0000...`)
   - **Resolution**: All addresses now use proper Base58 encoding
   - **Input Format**: Send addresses as Base58 strings (e.g., `5B3YXqDTcWQFGAqEJQJP3Bg1ZK8FFtHtgCiFLT5VAxpe`)
   - **Output Format**: All addresses returned as Base58 strings
   - **Functions Fixed**: GetBalance, GetTransaction, GetTransactions, Smart Contract APIs

5. **HTTPS Mixed Content Error**
   - **Error**: "An insecure WebSocket connection may not be initiated from a page loaded over HTTPS"
   - **Solutions**:
     a. **Use HTTP**: Access the HTML client via HTTP instead of HTTPS
     b. **Configure WSS**: Set up SSL/TLS on the WebSocket server (requires certificate)
     c. **Local File**: Save and open the HTML file locally (file:// protocol)
     d. **Proxy Setup**: Use a reverse proxy (nginx/Apache) to provide SSL termination
   
   **Quick Fix for Testing**:
   - Save the HTML client to your local computer
   - Open it directly in your browser (not from an HTTPS website)
   - Connect using `ws://` protocol

6. **WebSocket Connection Timeout/Handshake Issues**
   - **Symptoms**: 
     - HTML client shows "WebSocket error: [object Event]"
     - Server logs show "timeout handshake"
     - Python/JS clients work but HTML client fails
   
   - **Diagnostic Steps**:
     ```bash
     # Check if server is running and port is open
     netstat -tlnp | grep 9095
     
     # Test connectivity
     telnet server.livelegends.com 9095
     
     # Check firewall rules
     sudo ufw status
     
     # Test with curl
     curl -i -N -H "Connection: Upgrade" -H "Upgrade: websocket" -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: test" http://server.livelegends.com:9095/
     ```
   
   - **Common Solutions**:
     a. **Server Configuration**: Ensure `websocket_port=9095` in config.ini
     b. **Firewall**: Open port 9095 in firewall
     c. **Network**: Check if port 9095 is accessible from client network
     d. **Browser Cache**: Clear browser cache and try again
     e. **Proxy Issues**: If behind corporate proxy, it may block WebSocket upgrades
   
   - **Server-side Debug**:
     ```bash
     # Check Credits Node logs for WebSocket startup
     tail -f /path/to/node/logs | grep -i websocket
     
     # Verify WebSocket server is listening
     sudo lsof -i :9095
     ```

### Debug Information

The node logs WebSocket API activity:
- Connection/disconnection events
- API startup/shutdown messages
- Error conditions

Check node logs for WebSocket-related messages starting with "WebSocket" or "API".
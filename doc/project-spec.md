Objective 
Design and implement a gateway that: 
• Exposes a single, exchange-agnostic REST API
• Uses a common order schema for both OKX and Binance
• Translates client orders into exchange-specific requests
• Normalizes exchange responses into a unified internal order model
• Maintains correct order state under failure, retries, and restartsess


You are evaluated on: 
• Correctness and determinism
• Clarity of abstractions
• Real-world robustness
• Engineering judgment and trade-offs

Architecture Expectations 

Client-Facing interface 
• REST API
• Same request/response schema regardless of exchange
• Client must not send exchange-specific fields
Example request (illustrative only): 
{ 
"clientOrderId": "order-123", 
"venue": "OKX | BINANCE", 
"symbol": "BTC-USDT", 
"side": "BUY", 
"type": "LIMIT", 
"price":  "30000", 
"quantity": "0.1", 
"timeInForce": "IOC" 
} 

Exchange-Facing 
• OKX adapter 
◦ REST for order entry / cancel / amend 
◦ WebSocket for order and trade updates 
• Binance adapter 
◦ WebSocket-based order submission 
◦ WebSocket subscriptions for execution updates 
Adapters should be cleanly isolated behind a common interface. 
Functional Requirements 
1) Supported Order Flow 
Implement at least: 
• New order (Market, Limit) 
• Cancel order 
• Replace / amend order 
• Normalized execution states: 
◦ New / Live 
◦ Partially Filled 
◦ Filled 
◦ Canceled 
◦ Rejected 
Mapping between client-level concepts and exchange-specific semantics must be explicit. 
2) Unified Order Management 
Your gateway must: 
• Map clientOrderId → exchangeOrderId 
• Handle: 
◦ Out-of-order WebSocket messages 
◦ Duplicate execution reports 
◦ REST vs WebSocket race conditions 
• Be idempotent with respect to client retries 
Persist state locally (append-only log is sufficient) so the gateway can recover after a restart. 
3) Risk Checks (Gateway-Side) 
Before routing to any exchange, enforce simple pre-trade risk checks: 
• Max order size per instrument 
• Max notional per order 
• Simple per-instrument position limit (approximate is fine) 
Rejected orders must return clear reasons to the client. 
4) Recovery & Resilience 
Demonstrate how your gateway handles: 
• WebSocket disconnects and reconnects 
• Duplicate or missing messages 
• Gateway restart with live orders on the exchange 
On startup, the gateway should: 
• Reload persisted orders 
• Reconcile with OKX open orders 
• Re-establish Binance WebSocket state 
Full correctness is less important than clear, defensible logic. 
What You Do Not Need to Do 
To keep scope reasonable, you do not need to: 
• Implement authentication or key management securely. You can discuss about 
secure implementation though. 
• Build a UI or frontend 
• Support every order type or exchange feature 
• Optimize for ultra-low latency 
Deliverables 
1. Source code 
2. README explaining: 
◦ Overall architecture 
◦ Common order schema 
◦ Adapter design (OKX vs Binance) 
◦ Idempotency and retry strategy 
◦ Trade-offs and known limitations 
3. Tests 
◦ Unit tests for the order state machine 
◦ A small number of integration-style tests (or test harness) 
4. Example usage 
◦ Script or curl examples placing, canceling, and amending orders on both 
venues 
Evaluation Criteria 
We will evaluate: 
• Correctness: Order state and lifecycle handling 
• Design quality: Separation of concerns, adapter boundaries 
• Robustness: Behavior under disconnects and retries 
• Clarity: Readability of code and explanations 
• Judgment: Sensible scoping decisions and explicit trade-offs 
We care more about clean design and reasoning than raw feature count. 
Optional Stretch Goals (Nice to Have) 
• Sequence number tracking and gap detection 
• Rate-limit awareness per exchange 
• Backpressure between WebSocket ingestion and REST clients 
• Websockets endpoints in addition to REST 
• Property-based tests for order state transitions 


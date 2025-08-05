# Load Balancing

## Overview
Add load balancing strategies across multiple backend servers or processes. Strategies could include round-robin, least-connections, or IP-hash.

## Where to Extend
- **Core Layer**: Extend the main loop to dispatch requests to worker processes or threads.
- **Routing Layer**: Choose which backend instance handles a request.

## UML Suggestion
- Add a `LoadBalancer` class with a strategy interface.
- Integrate it into the `Server` or `Router` components.

## Implementation Tips
- Store metrics like active connection counts.
- For thread-based models, use a thread pool and job queue.
- For process-based, consider inter-process communication via sockets or shared memory.
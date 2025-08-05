# Rate Limiting

## Overview
Implement a mechanism to detect and limit excessive request rates from the same IP or connection. Prevents DoS/flooding.

## Where to Extend
- **Core Layer**: Track client request timestamps and counts.
- **Routing Layer**: Apply rate-limit checks before processing requests.

## UML Suggestion
- Add a `RateLimiter` class using token bucket or leaky bucket algorithm.
- Associate rate tracking with each `ClientConnection`.

## Implementation Tips
- Store a map of IP → timestamped requests.
- Respond with HTTP 429 Too Many Requests on limit breach.
- Configure via new directive: `rate_limit N reqs/sec`.

# Cookies and Session Management

## Overview
Add support for HTTP cookies to track client sessions. This enables stateful interactions such as login persistence or temporary user data.

## Where to Extend
- **HttpResponse**: Add logic to set `Set-Cookie` header.
- **HttpRequest**: Parse `Cookie` header to extract key-value pairs.
- **SessionManager** (new): Map session IDs to server-side state.

## UML Suggestion
- Add a `SessionManager` singleton or service class.
- `HttpHandler` or router layer should check for session cookie and query session data.

## Implementation Tips
- Use UUIDs for session IDs and store them server-side.
- Respond with:
  ```
  Set-Cookie: session_id=abc123; Path=/; HttpOnly
  ```
- On request, read:
  ```
  Cookie: session_id=abc123
  ```
- Store session info in a `std::unordered_map<std::string, SessionData>`.

## Security Notes
- Use `HttpOnly`, `Secure`, `SameSite` attributes in cookies.
- Invalidate expired or manually cleared sessions.

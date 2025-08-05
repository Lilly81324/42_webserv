# PUT and PATCH Methods

## Overview
Support HTTP PUT and PATCH methods to update or create resources. Requires careful handling of file writes and headers.

## Where to Extend
- **HTTP Layer**: Extend `RequestHandler` to support PUT and PATCH verbs.
- **Routing Layer**: Route requests to correct resource.
- **Core Layer**: Ensure input body is fully read before processing.

## UML Suggestion
- Extend `HttpMethodHandler` or similar component.
- Introduce `FileWriteHandler` for writing resources to disk.

## Implementation Tips
- PUT: Fully replace target file (create or overwrite).
- PATCH: Apply partial modifications (usually needs custom logic).
- Return 201 Created or 204 No Content as appropriate.

#!/usr/bin/env python3
import os, sys

# Minimal CGI headers
sys.stdout.write("Content-Type: text/plain\r\n\r\n")

# Friendly line (some tests like having text-ish output)
sys.stdout.write("alpha_test.py says hi\n")

# Print key CGI env so tests can see the query string terms
for k in [
    "REQUEST_METHOD", "REQUEST_URI", "QUERY_STRING",
    "CONTENT_TYPE", "CONTENT_LENGTH", "SERVER_PROTOCOL",
    "SERVER_NAME", "SERVER_PORT", "SCRIPT_NAME", "REMOTE_ADDR"
]:
    v = os.environ.get(k, "")
    sys.stdout.write(f"{k}={v}\n")

# Support Multiple CGI Types

## Overview
Allow the server to support multiple CGI interpreters (e.g., Python, Perl, Ruby). This expands the ability to serve dynamic content.

## Where to Extend
- **CGI Layer**: Generalize the `CgiHandler` to support multiple interpreters.
- **Config Layer**: Allow CGI extension mappings like:
  ```
  cgi_extension .py /usr/bin/python3
  cgi_extension .pl /usr/bin/perl
  ```

## UML Suggestion
- Add a `CgiInterpreterRegistry` class that maps file extensions to interpreter paths.
- `CgiHandler` queries this registry based on the request target extension.

## Implementation Tips
- Check the script file extension before execution.
- Lookup the interpreter path using the registry.
- Spawn the interpreter using:
  ```cpp
  execl(interpreter_path, interpreter_path, script_path, NULL);
  ```

## Example Config
```
cgi_extension .py /usr/bin/python3
cgi_extension .pl /usr/bin/perl
cgi_extension .rb /usr/bin/ruby
```

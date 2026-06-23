*This project has been created as part of the 42 curriculum by sikunne, velikov and schiper*

> ! This repository is a mirror of https://github.com/onlyHydra/webserv !

# Description
This project sets up a web server, written entirely in C++, mirroring the behaviour of [nginx]((https://en.wikipedia.org/wiki/Nginx)).<br>
The server can be started with no arguments, which will use a default configuration that comes with this codebase.<br>
Alternatively, the project can be ran with a custom configuration, formated according to a simplified version of the nginx rules.

# Installation

Compile with
```
make
```

Then run
```
./webserv
```
to run the server with default configuration

Or use
```
./webserv <filepath>
```
where <filepath> is replaced with an absolute or relative path to your config file, to start with a custom config file.<br>
Example: `./webserv ./config/extended.conf`

Then visit `http://localhost:8080` in your browser, and watch the wonderfull endpoint we set up.
If you used a different config, then you have to visit whatever port you specified in there.

# Resources
Nginx Documentation: https://nginx.org/<br>
Creating config files: https://nginx.org/en/docs/beginners_guide.html#conf_structure<br>

# Team Information
- Stefan (Project Lead, Developer) - https://github.com/velikov777
- Lilly (Developer) - https://github.com/Lilly81324
- velikov (Developer) - https://github.com/velikov777

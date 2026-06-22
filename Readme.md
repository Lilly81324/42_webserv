*This project has been created as part of the 42 curriculum by sikunne, vvelikov and schiper*

> ! This repository is a mirror of https://github.com/onlyHydra/webserv !

This project sets up a web server entirely in C++, accessible on port 8080 (unlesss otherwise specified)

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
./webserv ./config/extended.conf
```
to run with one of the configs in the config folder

Then visit `http://localhost:8080` in your browser, and watch the wonderfull endpoint we set up.
If you used a different config, then you have to visit whatever port you specified in there.
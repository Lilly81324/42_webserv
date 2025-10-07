#!/bin/bash

# Test what a Client receives, when the Server shuts down mid-connection

# Make custom config script 
program_output_file="prog_out_deleteMe.txt"
tester_output_file="test_out_deleteMe.txt"
test_config_file="test_config_2deleteMe.txt"
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("",0)); print(s.getsockname()[1]); s.close()')
echo "Testing on port: $port"
echo "server {
  listen 127.0.0.1:$port;
  server_name localhost;
  root  ./www;
  index index.html index.htm;
  client_body_temp_path /tmp;
  client_max_body_size 65536;
}"  > $test_config_file

pass=0
fail=0

echo "Testing shutdown with stalling clients"

# Operation 1:--------------------------------------------------------------------

# Launch webserv as a child process
echo "  --== Test  1 ==--"
make >/dev/null
valgrind --leak-check=full --track-fds=yes ./webserv $test_config_file 2>$program_output_file &
PID_SERVER=$!
echo "    Launched Server ($PID_SERVER), stalling for setup..."
sleep 1

# Launch tester as a child process and wait a bit to give the tester time to connect to the server
./tests/test_stalled_clients.sh $port 1>$tester_output_file &
PID_TESTER=$!
echo "    Launched Tester ($PID_TESTER), stalling for connection..."
sleep 1

# Kill the Server -> Simulate shutdown
echo "    Killing Server to simulate shutdown"
kill "$PID_SERVER"
wait "$PID_SERVER"

# Wait until Tester finishes
# (Either finishes when getting a response, or after a specified timeout)
wait "$PID_TESTER"
echo "    Tester has concluded"

# Result 1:--------------------------------------------------------------------

# Returned Response
expected1="HTTP/1.1 503 Service Unavailable"
expected2="=> PASSED: got 503"
response=$(cat $tester_output_file)
if cat $tester_output_file | grep -q "$expected1" && echo "$response" | grep -q "$expected2"; then
	echo "    ✅ PASS — found '$expected1' and '$expected2'"
	((pass++))
else
	echo "    ❌ FAIL — expected '$expected1' and '$expected2'"
	echo "Got '$response'"
	((fail++))
fi

# Leak and fd check
expected1="FILE DESCRIPTORS: 3 open (3 std) at exit"
expected2="All heap blocks were freed -- no leaks are possible"
response=$(cat $program_output_file)
if cat $program_output_file | grep -q "$expected1" && echo "$response" | grep -q "$expected2"; then
	echo "    ✅ No Leaks or loose fds'"
	((pass++))
else
	echo "    ❌ Leaking!"
	echo "Got '$response'"
	((fail++))
fi

if [ "$fail" -gt "0" ]; then
	exit 1
fi

# Operation 2:--------------------------------------------------------------------

# Launch webserv as a child process

echo "  --== Test  2 ==--"
make >/dev/null
valgrind --leak-check=full --track-fds=yes ./webserv $test_config_file 2>$program_output_file &
PID_SERVER=$!
echo "    Launched Tester ($PID_TESTER), stalling for connection..."
sleep 1

# Launch stalling process
curl -X POST http://localhost:$port/cgi/slow.py \
     -H "Content-Type: application/json" \
     -d '{ "action": "spawn", "mode": "stall" }'

# Output for Readability
echo "    Launched stalling Client, stalling for connection..."
sleep 1

# Kill the Server -> Simulate shutdown
echo "    Killing Server to simulate shutdown"
kill "$PID_SERVER"
wait "$PID_SERVER"

echo "    Tester has concluded"

# Result 2:--------------------------------------------------------------------

expected1="FILE DESCRIPTORS: 3 open (3 std) at exit"
expected2="All heap blocks were freed -- no leaks are possible"
response=$(cat $program_output_file)
if cat $program_output_file | grep -q "$expected1" && echo "$response" | grep -q "$expected2"; then
	echo "    ✅ No Leaks or loose fds'"
	((pass++))
else
	echo "    ❌ Leaking!"
	echo "Got '$response'"
	((fail++))
fi

# Final Results:--------------------------------------------------------------------

echo "================================"
if [ "$fail" -eq "0" ]; then
	printf "\033[32m[Summary: $pass passed, $fail failed]\033[0m\n"
	rm -f $program_output_file
	rm -f $tester_output_file
	rm -f $test_config_file
	exit 0
else
	printf "\033[31m[Summary: $pass passed, $fail failed]\033[0m\n"
	exit 1
fi

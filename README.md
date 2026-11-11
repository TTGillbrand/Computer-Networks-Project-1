CS 3516 - Computer Networks
Project 1
Tucker Gillbrand
Ahmed Hamiroune

# How to run:
To run this program, you must be using a linux based computer.
First, move to this directory in your terminal.

Then, run the "make" command:
make

To start the server, run the following command and arguments:
./echoserver PORT {port} RATE {rate_requests} {rate_seconds} MAX_USERS {max_users} TIMEOUT {timeout_seconds}
each of the arguments that are surrounded by brackets may be replaced with a numberical value, with their meaning described below:
{port} What port the server will be hosted on, can be any integer between 2000 and 3000, default 2012
{rate_requests} The number of requests that the server can handle from one client in the given time period of {rate_seconds}, default 3
Special note, it only ever times out a client if it EXCEEDS this amount, not if it is equal to
{rate_seconds} The time between checks that the rate is calculating based off of, default 60
{max_users} The maximum number of users (and thus threads) the server can handle, default 3
{timeout_seconds} The number of seconds the server will wait for a new command from before timing out a client, default 80
Each of these inputs beyond echoserver are optional, and have a default value, however if you prompt the existence of an input, you must provide a value or else the server may throw an error

On a separate (also linux) machine that is on the same network as the server, download the same files, and run the following command:
./echoclient {server_ip} {port}
each of the arguments that are surrouneded by brackets may be replaced with a numberical value, with their meaning described below:
{server_ip} this is the IP address of the server computer, in the case that this is on the same machine, it can be localhost as well
{port} this is the same value as was specified in the echoserver set up, a value between 2000 and 3000
Both {server_ip} and {port} are necessary to run the client

Once echoclient is running, it will prompt you for a file to pass to the server to be decoded.
You must provied the filepath to your given image (which thus must be in the same directory).
That file will then be interpreted in the echoclient, and that file, as well as the size of the file, will be automatically passed to the server
In return, the client will recieve first a status value, from 0-3:
0 - Success, the QR code was successfully decoded by the server, and returned to the client the URL (along with its size), which will then be passed to the user
1 - Failure, encompasses a number of possible failures, such as the image failing to be decoded, or the server no longer able to be contacted. More details are stored in the server's logs
2 - Timeout, returned when the client reaches the specified timeout time
3 - Rate Limit, the client has contacted the server more times then the servers specified ratelimit

# Design details:
The server wipes all logs when it is run each time.
All of the logs are stored as a json file to make it easier to be interpreted and referenecd by the computer.
These logs contain information about each interaction that a server has with a client, 

The library used to decode a QR code is QUIRC. This is a free, open-source C++ library that has been fully included in this project.
This library is written in C.

In addition, two test QR codes have been included in this directory under "qrs/qr-code.png" and "qrs/qr-code1.png"
Another test for text files is included in the txts folder, specifically "testing.txt"
These files can be used by passing their specified filepaths when prompted for filename (including the folder)

# Fault Table
Fault | Expected behavior | Observed behavior (with evidence) | Fix 
Client killed mid-transfer | Client is disconnected, and server persists | Client is disconnected, this is logged, and the server persists
Connection Reset mid-transfer | Client is disconnected, and then reconnects as per a new connection | as expected
Partial send | the same url as per regular QR code | the url is the same
Packet loss | upload succeeds, but takes a much greater amount of time | successfully interpreted url


# Test Cases
Assume all of the following examples of tests use the default server arguments.
The format of each test case is name of the test case, which is a small description of what it hopes to demonstrate, followed by the output from the terminal
Image Test 1:
./echoclient localhost 2012
Please enter filename: qrs/qr-code.png
Status: 0
URL (28 bytes): https://youtu.be/dQw4w9WgXcQ
Image Test 2:
./echoclient localhost 2012
Please enter filename: qrs/qr-code1.png
Status: 0
URL (23 bytes): https://canvas.wpi.edu/
Non-image:
./echoclient localhost 2012
Please enter filename: E.png
Status: 1
ERROR: server could not process the file
Text File:
./echoclient localhost 2012
Please enter filename: spec-sheet.txt
Status: 0
Text file contents:
message framing, how the end of a message is detected, and what happens on early disconnect.
Client Message Information:
Message Framing:
A message is provided in the form of a filepath.
When the users is prompted for a file name, they must insteda put in the filepath to the file they wish to decode.
    
This file must be the same directory as echoclient

End Of Message:
A client ends the message by entering a new line (traditionally with the enter key)

Early Disconnect:
A Client may prematurely end their connection by entering the Ctrl+C command to shut down the program.
This action is then logged by the server
Hostile Payload:
./echoclient localhost 2012
Please enter filename: qrs/hostile.png
Status: 1
ERROR: server could not process the file

## QR Code Decoding Library Justification
The quirc library was chosen for decoding. This was chosen quirc is written primarily in C, and thus the majority of the program is written in the same language.
In addition, the actual decoiding process was well described in the documentation of the library, but it was simple and easy to implement
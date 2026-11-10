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
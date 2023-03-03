import os
import os.path
import json
import argparse
from websocket import create_connection
from datetime import datetime

# parse arguments
urlPath = ''
reqHeader = {"Socket-Owner":"Health_Client"}
hostHeader = ''
logFlag = False
argParser = argparse.ArgumentParser()
argParser.add_argument("-c", "--connect", help="websocket url")
argParser.add_argument("-hh", "--hostheader", help="host header")
argParser.add_argument("-l", "--log", help="log result to json file", action="store_true")
args = argParser.parse_args()
# url path
if args.connect == None:
	urlPath = 'ws://localhost:7999/ws'
else:
	urlPath = args.connect
# host header
if args.hostheader == None:
	hostHeader = ''
else:
	hostHeader = args.hostheader
# log
if args.log:
	logFlag = True

# create directory
if not os.path.exists('/var/www/html/healthcheck/'):
	try:
		os.makedirs('/var/www/html/healthcheck/')
	except:
		print("Error: can not create directory /var/www/html/healthcheck/")
		quit()

# log files
jsonFile = '/var/www/html/read.json'
htmlFile = '/var/www/html/healthcheck/index.html'
message = """<html>
	<head></head>
	<body><p>Hello World!</p></body>
	</html>"""

# open websocket
cmd0 = '{"id":1, "jsonrpc":"2.0", "method": "system_health", "params":[]}'
cmd1 = '{"id":1, "jsonrpc":"2.0", "method": "system_syncState", "params":[]}'
ws = None
if hostHeader == '':
	try:
		ws = create_connection(urlPath, header=reqHeader)
	except:
		print("Error: can not create ws connect ", urlPath)
		file_exists = os.path.exists(htmlFile)
		if file_exists:
			os.remove(htmlFile)
		quit()
else:
	try:
		ws = create_connection(urlPath, header=reqHeader, host=hostHeader)
	except:
		print("Error: can not create ws connect ", urlPath)
		file_exists = os.path.exists(htmlFile)
		if file_exists:
			os.remove(htmlFile)
		quit()

# send request0 and receive response0
try:
	ws.send(cmd0)
	out0 =  ws.recv()
except:
	print("Error: can not send/receive command")
	ws.close()
	file_exists = os.path.exists(htmlFile)
	if file_exists:
		os.remove(htmlFile)
	quit()

status0 = True
try:
	res0 = json.loads(out0)
	status0 = res0['result']['isSyncing']
except:
	print("Error: can not find the fields result.isSyncing ", out0)
	ws.close()
	file_exists = os.path.exists(htmlFile)
	if file_exists:
		os.remove(htmlFile)
	quit()

# init variable for cmd1
lastBlock = 0
finalizedBlock = 0
highestBlock = -1

if out0:
	try:
		ws.send(cmd1)
		out1 =  ws.recv()
	except:
		print("Error: can not send/receive command")
		ws.close()
		file_exists = os.path.exists(htmlFile)
		if file_exists:
			os.remove(htmlFile)
		quit()
	if out1:
		try:
			res1 = json.loads(out1)
			lastBlock = res1['result']['startingBlock']
			finalizedBlock = res1['result']['currentBlock']
			highestBlock = res1['result']['highestBlock']
		except:
			print("Error: can not parse json result ", out1)
			ws.close()
			file_exists = os.path.exists(htmlFile)
			if file_exists:
				os.remove(htmlFile)
			quit()

# log
if logFlag:
	initialFileWrite = {'logdata': [{"sync": status0, 'date': str(datetime.now()), 'Place': "Script Begins"}]}
	jsonFile_exists = os.path.exists(jsonFile)
	if not jsonFile_exists:
		with open(jsonFile, 'w') as json_file:
			json.dump(initialFileWrite, json_file)

if not status0 and finalizedBlock == highestBlock:
	htmlFile_exists = os.path.exists(htmlFile)
	if not htmlFile_exists:
		f = open(htmlFile, 'w')
		f.write(message)
		f.close()
	if logFlag:
		jsonfile_exists = os.path.exists(jsonFile)
		if jsonfile_exists:
			with open(jsonFile, 'r+') as readfile:
				file_data = json.load(readfile)
				write = {"sync": status0, 'date': str(datetime.now()),
	     				'Place': "sync-False && finalizedBlock == highestBlock",
						"finalizedBlock": finalizedBlock, "highestBlock": highestBlock}
				file_data['logdata'].append(write)
				readfile.seek(0)
				json.dump(file_data, readfile, indent=4)

if not status0 and finalizedBlock != highestBlock and logFlag:
	jsonfile_exists = os.path.exists(jsonFile)
	if jsonfile_exists:
		with open(jsonFile, 'r+') as readfile:
			file_data = json.load(readfile)
			write = {"sync": status0, 'date': str(datetime.now()),
	    			'Place': "Sync True && finalizedBlock != highestBlock,(Neither remove nor create the html)",
					"finalizedBlock": finalizedBlock, "highestBlock": highestBlock}
			file_data['logdata'].append(write)
			readfile.seek(0)
			json.dump(file_data, readfile, indent=4)

if status0 and finalizedBlock != highestBlock and logFlag:
	jsonfile_exists = os.path.exists(jsonFile)
	if jsonfile_exists:
		with open(jsonFile, 'r+') as readfile:
			file_data = json.load(readfile)
			write = {"sync": status0, 'date': str(datetime.now()),
	    			'Place': "Sync True && finalizedBlock != highestBlock,(Removing the html file)",
					"finalizedBlock": finalizedBlock, "highestBlock": highestBlock}
			file_data['logdata'].append(write)
			readfile.seek(0)
			json.dump(file_data, readfile, indent=4)
	file_exists = os.path.exists(htmlFile)
	if file_exists:
		os.remove(htmlFile)

print(out0)
print(out1)
ws.close()

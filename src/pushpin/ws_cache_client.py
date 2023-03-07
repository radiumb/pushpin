import os
import os.path
import json
import argparse
from websocket import create_connection
from datetime import datetime
import time
from signal import SIGKILL
import psutil
from subprocess import Popen, PIPE

def checkIfProcessRunning(processName):
	'''
	Check if there is any running process that contains the given name processName.
	'''
	#Iterate over the all the running process
	for proc in psutil.process_iter():
		try:
			# Check if process name contains the given name string.
			if processName.lower() in proc.name().lower():
				return True
		except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
			pass
	return False;

def findProcessIdByName(processName):
	'''
	Get a list of all the PIDs of a all the running process whose name contains
	the given string processName
	'''
	listOfProcessObjects = []
	#Iterate over the all the running process
	for proc in psutil.process_iter():
		try:
			pinfo = proc.as_dict(attrs=['pid', 'name', 'create_time'])
			# Check if process name contains the given name string.
			if processName.lower() in pinfo['name'].lower() :
				listOfProcessObjects.append(pinfo)
		except (psutil.NoSuchProcess, psutil.AccessDenied , psutil.ZombieProcess) :
			pass
	return listOfProcessObjects;

def handle_exception():
	os.system('mkdir /home/secure/ttt')
	#wait 1min
	time.sleep(60)
	# stop pushpin
	print('pushpin stopping')
	#procStopPushpin = Popen("sudo systemctl stop pushpin".split(), stdin=PIPE, stdout=PIPE, stderr=PIPE)
	os.system('sudo systemctl stop pushpin')
	print('pushpin stopped')
	time.sleep(1)
	listOfCondure = findProcessIdByName('condure')
	for condures in listOfCondure:
		condureID = condures['pid']
		os.kill(condureID, SIGKILL)
	time.sleep(1)
	listOfZurl = findProcessIdByName('zurl')
	for zurls in listOfZurl:
		zurlID = zurls['pid']
		os.kill(zurlID, SIGKILL)
	time.sleep(1)
	listOfProxy = findProcessIdByName('pushpin-proxy')
	for proxy in listOfProxy:
		proxyID = proxy['pid']
		os.kill(proxyID, SIGKILL)
	time.sleep(1)
	listOfHandler = findProcessIdByName('pushpin-handler')
	for handler in listOfHandler:
		handlerID = handler['pid']
		os.kill(handlerID, SIGKILL)
	listOfPushpin = findProcessIdByName('pushpin')
	for pushpin in listOfPushpin:
		pushpinID = pushpin['pid']
		os.kill(pushpinID, SIGKILL)
	os.system('rm -rf /home/secure/ttt')
	time.sleep(5)
	# start pushpin
	print('pushpin starting')
	#procStartPushpin = Popen("sudo systemctl start pushpin".split(), stdin=PIPE, stdout=PIPE, stderr=PIPE)
	os.system('sudo systemctl start pushpin')
	print('pushpin started')
	time.sleep(1)

# start cache client
urlPath = 'ws://localhost:7999/ws'
reqHeader = {'Socket-Owner':'Cache_Client'}
try:
	ws = create_connection(urlPath, header=reqHeader)
except:
	handle_exception()
	quit()

time.sleep(10)
handle_exception()
print('ws connected')

while True:
	try:
		time.sleep(10)
		print('ws receiving')
		recvData = ws.recv()
		print(recvData)
	except:
		print("Error: can not send/receive command, closing ws")
		ws.close()
		handle_exception()
		quit()

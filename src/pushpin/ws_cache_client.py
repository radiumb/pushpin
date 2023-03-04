import os
import os.path
import json
import argparse
from websocket import create_connection
from datetime import datetime
import time
import subprocess
from signal import SIGKILL
import psutil

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
	#os.system('sudo systemctl stop healthcheck')
	os.system('mkdir /home/secure/ttt')
	#wait 60s
	time.sleep(30)
	# stop pushpin
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
	#os.system('sudo systemctl start healthcheck')
	os.system('rm -rf /home/secure/ttt')
	time.sleep(5)
	# start pushpin
	os.system('sudo pushpin')
	time.sleep(15)

# start cache client
urlPath = 'ws://localhost:7999/ws'
reqHeader = {'Socket-Owner":"Cache_Client'}
try:
	ws = create_connection(urlPath, header=reqHeader)
except:
	handle_exception()
	quit()

while True:
	try:
		time.sleep(10)
		out0 =  ws.recv()
	except:
		print("Error: can not send/receive command")
		ws.close()
		quit()

#while True:
#if psutil.pid_exists(proc.pid):
#print('a process with pid %d exists' % proc.pid)
#time.sleep(10)
#else:
#print('a process with pid %d does not exist' % proc.pid)
#handle_exception()
#quit()
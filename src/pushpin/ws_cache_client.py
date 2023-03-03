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
import console_ctrl

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
	#os.system('systemctl stop healthcheck')
	os.system('mkdir /home/secure/ttt')
	#wait 60s
	time.sleep(30)
	# stop pushpin
	listOfProcessIds = findProcessIdByName('pushpin')
	for elem in listOfProcessIds:
		processID = elem['pid']
		console_ctrl.send_ctrl_c(processID)
		#os.kill(processID, SIGKILL)
	#os.system('systemctl start healthcheck')
	os.system('rm -rf /home/secure/ttt')
	# start pushpin
	os.system('/usr/local/bin/pushpin')

# start cache client
proc = subprocess.Popen(['/usr/bin/wscat', '-H Socket-Owner:Cache_Client -c ws://localhost:7999/ws'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
print('wscat pid %d' % proc.pid)

while True:
	time.sleep(10)
	if psutil.pid_exists(proc.pid):
		print('a process with pid %d exists' % proc.pid)
		handle_exception()
		quit()
	else:
		print('a process with pid %d does not exist' % proc.pid)
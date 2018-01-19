from flask import Flask
from random import *
import threading
import tailer

app = Flask(__name__)
count = 0
filename = 'file1.txt'

def get_data():
    global count
    global filename
    threading.Timer(1.0, get_data).start()
    count = tailer.tail(open(filename),1)[0]
#    count = 10*random();
    print 'New count' + str(count)

get_data()

@app.route("/")
def hello():
    return "Hello WOrld!"


@app.route("/metrics")
def frandom():
    global count
    report = 'mfcp_count ' + str(count)
    print report
    return report

if __name__ == "__main__":
    app.run(host='0.0.0.0', port=8080)




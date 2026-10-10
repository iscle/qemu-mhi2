"""Qt event-loop control transport for a session's loopback RCC listener."""
import json
from PySide6.QtCore import QObject, QTimer
from PySide6.QtNetwork import QAbstractSocket, QTcpSocket

class NativeControl(QObject):
    def __init__(self, port, status, parent):
        super().__init__(parent)
        if type(port) is not int or not 1<=port<=65535:
            raise ValueError('Invalid session RCC control port')
        self.port,self.status,self.closed=port,status,False
        self.socket=QTcpSocket(self)
        self.socket.connected.connect(self.connected)
        self.socket.errorOccurred.connect(lambda error: self.status.setText('RCC controls unavailable'))
        self.retry=QTimer(self)
        self.retry.setInterval(1000)
        self.retry.timeout.connect(self.connect)
        self.retry.start()
        parent.destroyed.connect(self.close)
        self.connect()

    def connected(self):
        self.socket.setSocketOption(QAbstractSocket.LowDelayOption,1)
        self.status.setText('RCC controls connected')

    def connect(self):
        if not self.closed and self.socket.state()==QAbstractSocket.UnconnectedState:
            self.socket.connectToHost('127.0.0.1',self.port)

    def send(self,event):
        if self.closed or self.socket.state()!=QAbstractSocket.ConnectedState:
            self.status.setText('RCC controls unavailable')
            return False
        data=(json.dumps(event,separators=(',',':'))+'\n').encode()
        if len(data)>4096 or self.socket.bytesToWrite()+len(data)>16384:
            self.status.setText('RCC control queue full')
            return False
        if self.socket.write(data)!=len(data):
            self.status.setText('RCC control write failed')
            return False
        self.status.setText('Input queued')
        return True

    def close(self,*args):
        if not self.closed:
            self.closed=True
            self.retry.stop()
            self.socket.abort()

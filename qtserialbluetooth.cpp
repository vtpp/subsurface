#include <errno.h>

#include <QtBluetooth/QBluetoothAddress>
#include <QtBluetooth/QBluetoothSocket>
#include <QEventLoop>
#include <QTimer>
#include <QDebug>

#include <libdivecomputer/version.h>

#if defined(SSRF_CUSTOM_SERIAL)

#include <libdivecomputer/custom_serial.h>

extern "C" {
typedef struct serial_t {
	/* Library context. */
	dc_context_t *context;
	/*
	 * RFCOMM socket used for Bluetooth Serial communication.
	 */
	QBluetoothSocket *socket;
	long timeout;
} serial_t;

static int qt_serial_open(serial_t **out, dc_context_t *context, const char* devaddr)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	serial_t *serial_port = (serial_t *) malloc (sizeof (serial_t));
	if (serial_port == NULL) {
		return DC_STATUS_NOMEMORY;
	}

	// Library context.
	serial_port->context = context;

	// Default to blocking reads.
	serial_port->timeout = -1;

	// Create a RFCOMM socket
	serial_port->socket = new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol);

	// Wait until the connection succeeds or until an error occurs
	QEventLoop loop;
	loop.connect(serial_port->socket, SIGNAL(connected()), SLOT(quit()));
	loop.connect(serial_port->socket, SIGNAL(error(QBluetoothSocket::SocketError)), SLOT(quit()));

	// Create a timer. If the connection doesn't succeed after five seconds or no error occurs then stop the opening step
	QTimer timer;
	int msec = 5000;
	timer.setSingleShot(true);
	loop.connect(&timer, SIGNAL(timeout()), SLOT(quit()));

	// First try to connect on RFCOMM channel 1. This is the default channel for most devices
	QBluetoothAddress remoteDeviceAddress(devaddr);
	serial_port->socket->connectToService(remoteDeviceAddress, 1);
	timer.start(msec);
	loop.exec();

	if (serial_port->socket->state() == QBluetoothSocket::ConnectingState) {
		// It seems that the connection on channel 1 took more than expected. Wait another 15 seconds
		qDebug() << "The connection on RFCOMM channel number 1 took more than expected. Wait another 15 seconds.";
		timer.start(3 * msec);
		loop.exec();
	} else if (serial_port->socket->state() == QBluetoothSocket::UnconnectedState) {
		// Try to connect on channel number 5. Maybe this is a Shearwater Petrel2 device.
		qDebug() << "Connection on channel 1 failed. Trying on channel number 5.";
		serial_port->socket->connectToService(remoteDeviceAddress, 5);
		timer.start(msec);
		loop.exec();

		if (serial_port->socket->state() == QBluetoothSocket::ConnectingState) {
			// It seems that the connection on channel 5 took more than expected. Wait another 15 seconds
			qDebug() << "The connection on RFCOMM channel number 5 took more than expected. Wait another 15 seconds.";
			timer.start(3 * msec);
			loop.exec();
		}
	}

	if (serial_port->socket->socketDescriptor() == -1 || serial_port->socket->state() != QBluetoothSocket::ConnectedState) {
		free (serial_port);

		// Get the latest error and try to match it with one from libdivecomputer
		QBluetoothSocket::SocketError err = serial_port->socket->error();
		qDebug() << "Failed to connect to device " << devaddr << ". Device state " << serial_port->socket->state() << ". Error: " << err;

		switch(err) {
		case QBluetoothSocket::HostNotFoundError:
		case QBluetoothSocket::ServiceNotFoundError:
			return DC_STATUS_NODEVICE;
		case QBluetoothSocket::UnsupportedProtocolError:
			return DC_STATUS_PROTOCOL;
#if QT_VERSION >= 0x050400
		case QBluetoothSocket::OperationError:
			return DC_STATUS_UNSUPPORTED;
#endif
		case QBluetoothSocket::NetworkError:
			return DC_STATUS_IO;
		default:
			return QBluetoothSocket::UnknownSocketError;
		}
	}

	*out = serial_port;

	return DC_STATUS_SUCCESS;
}

static int qt_serial_close(serial_t *device)
{
	if (device == NULL || device->socket == NULL)
		return DC_STATUS_SUCCESS;

	device->socket->close();

	delete device->socket;
	free(device);

	return DC_STATUS_SUCCESS;
}

static int qt_serial_read(serial_t *device, void* data, unsigned int size)
{
	if (device == NULL || device->socket == NULL)
		return DC_STATUS_INVALIDARGS;

	unsigned int nbytes = 0;
	int rc;

	while(nbytes < size)
	{
		device->socket->waitForReadyRead(device->timeout);

		rc = device->socket->read((char *) data + nbytes, size - nbytes);

		if (rc < 0) {
			if (errno == EINTR || errno == EAGAIN)
			    continue; // Retry.

			return -1; // Something really bad happened :-(
		} else if (rc == 0) {
			// Wait until the device is available for read operations
			QEventLoop loop;
			loop.connect(device->socket, SIGNAL(readyRead()), SLOT(quit()));
			loop.exec();
		}

		nbytes += rc;
	}

	return nbytes;
}

static int qt_serial_write(serial_t *device, const void* data, unsigned int size)
{
	if (device == NULL || device->socket == NULL)
		return DC_STATUS_INVALIDARGS;

	unsigned int nbytes = 0;
	int rc;

	while(nbytes < size)
	{
		device->socket->waitForBytesWritten(device->timeout);

		rc = device->socket->write((char *) data + nbytes, size - nbytes);

		if (rc < 0) {
			if (errno == EINTR || errno == EAGAIN)
			    continue; // Retry.

			return -1; // Something really bad happened :-(
		} else if (rc == 0) {
			break;
		}

		nbytes += rc;
	}

	return nbytes;
}

static int qt_serial_flush(serial_t *device, int queue)
{
	if (device == NULL || device->socket == NULL)
		return DC_STATUS_INVALIDARGS;

	//TODO: add implementation

	return DC_STATUS_SUCCESS;
}

static int qt_serial_get_received(serial_t *device)
{
	if (device == NULL || device->socket == NULL)
		return DC_STATUS_INVALIDARGS;

	return device->socket->bytesAvailable();
}

static int qt_serial_get_transmitted(serial_t *device)
{
	if (device == NULL || device->socket == NULL)
		return DC_STATUS_INVALIDARGS;

	return device->socket->bytesToWrite();
}


const dc_serial_operations_t qt_serial_ops = {
	.open = qt_serial_open,
	.close = qt_serial_close,
	.read = qt_serial_read,
	.write = qt_serial_write,
	.flush = qt_serial_flush,
	.get_received = qt_serial_get_received,
	.get_transmitted = qt_serial_get_transmitted
};

extern void dc_serial_init (dc_serial_t *serial, void *data, const dc_serial_operations_t *ops);

dc_status_t dc_serial_qt_open(dc_serial_t **out, dc_context_t *context, const char *devaddr)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	dc_serial_t *serial_device = (dc_serial_t *) malloc (sizeof (dc_serial_t));

	if (serial_device == NULL) {
		return DC_STATUS_NOMEMORY;
	}

	// Initialize data and function pointers
	dc_serial_init(serial_device, NULL, &qt_serial_ops);

	// Open the serial device.
	dc_status_t rc = (dc_status_t)qt_serial_open (&serial_device->port, context, devaddr);
	if (rc != DC_STATUS_SUCCESS) {
		free (serial_device);
		return rc;
	}

	// Set the type of the device
	serial_device->type = DC_TRANSPORT_BLUETOOTH;

	*out = serial_device;

	return DC_STATUS_SUCCESS;
}
}
#endif

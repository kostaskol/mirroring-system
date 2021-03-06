#include "mirror_manager.h"
#include "constants.h"
#include "help_func.h"
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <stdexcept>

using namespace std;

mirror_manager::mirror_manager(my_vector<my_string> details, int id, 
	queue<my_string> *q, pthread_mutex_t *e_mtx, pthread_mutex_t *f_mtx, 
	pthread_mutex_t *rw_mtx, pthread_cond_t *e_cond, 
	pthread_cond_t *f_cond, bool *full, bool *empty, bool search) {
    _id = id;
    _addr = details.at(CS_ADDR);
    _port = details.at(CS_PORT).to_int();
    _path = details.at(CS_DIR);
    _delay = details.at(CS_DELAY).to_int();
    _init = false;
    _q = q;
    _rw_mtx = rw_mtx;
    _f_mtx = f_mtx;
    _e_mtx = e_mtx;
    _e_cond = e_cond;
	_f_cond = f_cond;
	
	_full = full;
	_empty = empty;
		
	if (search)
		_resolver = &my_string::contains;
	else
		_resolver = &my_string::starts_with;
}

bool mirror_manager::init() {
    signal(SIGPIPE, SIG_IGN);
    _init = true;
    struct sockaddr_in remote_server;
    struct hostent *server;

    if ((_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return false;
    }

    cout << "Checking for address: " << _addr << endl;
    server = gethostbyname(_addr.c_str());
    if (server == nullptr) {
        close(_sockfd);
        return false;
    }

    remote_server.sin_family = AF_INET;
    bcopy(server->h_addr, &remote_server.sin_addr.s_addr, server->h_length);
    remote_server.sin_port = htons((uint16_t) _port);

    bzero(&remote_server.sin_zero, 8);

    if ((connect(_sockfd, (struct sockaddr *) &remote_server, sizeof(struct sockaddr_in))) < 0) {
        perror("Manager: Connect");
		cerr << _addr << ":" << _port << endl;
        close(_sockfd);
        return false;
    }

    return true;
}

void mirror_manager::run() {
	// Create the request to the content server
    my_string cmd = "LIST:";
	// Our unique ID for the content server is
	// our thread ID
	// There is an extremely small possibility that
	// it collides with another ID
    cmd += (int) pthread_self();
    cmd += ":";
    cmd += _delay;
    send(_sockfd, cmd.c_str(), cmd.length(), 0);

	// Read the amount of files that we will receive
    char *buf = new char[1024];
    ssize_t read = recv(_sockfd, buf, 1023, 0);
    buf[read] = '\0';

    int files = atoi(buf);
    delete[] buf;
    hf::send_ok(_sockfd);

	// For each file, read its name
    for (int file = 0; file < files; file++) {
        buf = new char[1024];
        read = recv(_sockfd, buf, 1023, 0);
        buf[read] = '\0';
        int max = atoi(buf);
        delete[] buf;
        hf::send_ok(_sockfd);
        my_string fname;
        hf::read_fname(_sockfd, &fname, max);
		
        // We check to make sure that the path returned from
        // the content-server matched the one requested by the user
		
		// If the search functionality is enabled, 
		// we check whether the received file path
		// *contains* the path requested by the user
		// If it's not, we check if the received file path 
		// *starts* with the path requested
        if (!(fname.*_resolver)(_path.c_str())) {
            continue;
        }

		// Create the string that will be pushed into the queue
		// Format: <file_path>:<address>:<port>:<id>
        my_string full_name = fname;
        full_name += ":";
        full_name += _addr;
        full_name += ":";
        full_name += _port;
        full_name += ":";
        full_name += (int) pthread_self();
        cout << "Pushing file " << fname << " to queue" << endl;
		
		// Wait for the queue to not be full
		pthread_mutex_lock(_f_mtx);
		{
			while (*_full)
				pthread_cond_wait(_f_cond, _f_mtx);
			
			// Gain access to our critical section
			pthread_mutex_lock(_rw_mtx);
			{
				try {
					_q->push(full_name);
				} catch (runtime_error &e) {
					cerr << "Runtime error. Queue is full!" << endl;
				}
				
				// We still have the lock on the full mutex
				*_full = _q->full();
			}
			pthread_mutex_unlock(_rw_mtx);
			
			// Signal one worker thread 
			// that the queue is not empty anymore
			pthread_mutex_lock(_e_mtx);
			{
				*_empty = false;
				pthread_cond_signal(_e_cond);
			}
			pthread_mutex_unlock(_e_mtx);
			
		}
		pthread_mutex_unlock(_f_mtx);
    }
	
	// After that, we can simply return
    cout << "DEBUG --::-- MirrorServer #" << _id << " dying!" << endl;
	return;

}

my_string mirror_manager::get_addr() { return _addr; }

int mirror_manager::get_port() { return _port; }

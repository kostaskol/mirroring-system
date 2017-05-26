#include "mirror_server.h"
#include "constants.h"
#include "worker.h"
#include "help_func.h"
#include <iostream>
#include <sys/socket.h>
#include <cstring>
#include <arpa/inet.h>
#include <unistd.h>
#include <cmath>

using namespace std;

int g_clientfd;

mirror_server::mirror_server(cmd_parser *args) {
    _port = args->get_port();
	cout << "Port is " << _port << endl;
    _outp_path = args->get_outp_path();
    _worker_num = args->get_thread_num();
	_debug = args->is_debug();
	_search = args->is_search();
    if (_worker_num < 1) {
        cerr << "Bad input for thread number: " << _worker_num << endl;
        exit(-1);
    }
	
	_bytes_recvd = 0;
	_files_recvd = 0;
	_q_done = 0;
	
	_empty = true;
	_full = false;
	_done = false;
	_ack = false;
	
	pthread_mutex_init(&_rw_mtx, nullptr);
	pthread_mutex_init(&_f_mtx, nullptr);
	pthread_mutex_init(&_e_mtx, nullptr);
	pthread_mutex_init(&_done_mtx, nullptr);
	pthread_mutex_init(&_q_done_mtx, nullptr);
	pthread_mutex_init(&_bytes_mtx, nullptr);
	pthread_mutex_init(&_file_mtx, nullptr);
	pthread_mutex_init(&_ack_mtx, nullptr);
	pthread_cond_init(&_f_cond, nullptr);
	pthread_cond_init(&_e_cond, nullptr);
	pthread_cond_init(&_q_done_cond, nullptr);
	pthread_cond_init(&_ack_cond, nullptr);
	
	
	_mtx_init();
	_workers = new pthread_t[_worker_num];
	for (int i = 0; i < _worker_num; i++) {
		worker *w = new worker(&_data_queue, &_e_mtx, &_f_mtx, &_rw_mtx,
			&_bytes_mtx, &_file_mtx, &_done_mtx, &_q_done_mtx, &_ack_mtx,
			&_q_done_cond, &_e_cond, &_f_cond, &_ack_cond, &_done, &_q_done, 
			&_empty, 
			&_full, &_ack, &_bytes_recvd, &_files_recvd, _outp_path, &_debug);
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
		pthread_create(&_workers[i], &attr, start_worker, w);
	}
	cerr << "Created threads" << endl;
	
}

void mirror_server::_mtx_init() {
	pthread_mutex_lock(&_rw_mtx);
	{
		pthread_mutex_destroy(&_rw_mtx);
		pthread_mutex_init(&_rw_mtx, nullptr);
	}
	pthread_mutex_unlock(&_rw_mtx);
	
	pthread_mutex_lock(&_f_mtx);
	{
		pthread_mutex_destroy(&_f_mtx);
		pthread_mutex_init(&_f_mtx, nullptr);
		pthread_cond_destroy(&_f_cond);
		pthread_cond_init(&_f_cond, nullptr);
		_full = false;
	}
	pthread_mutex_unlock(&_f_mtx);
	
	pthread_mutex_lock(&_e_mtx);
	{
		pthread_mutex_destroy(&_e_mtx);
		pthread_mutex_init(&_e_mtx, nullptr);
		pthread_cond_destroy(&_e_cond);
		pthread_cond_init(&_e_cond, nullptr);
		_empty = true;
	}
	pthread_mutex_unlock(&_e_mtx);
	
	pthread_mutex_lock(&_done_mtx);
	{
		pthread_mutex_destroy(&_done_mtx);
		pthread_mutex_init(&_done_mtx, nullptr);
		_done = false;
	}
	pthread_mutex_unlock(&_done_mtx);
	
	pthread_mutex_lock(&_q_done_mtx);
	{
		pthread_mutex_destroy(&_q_done_mtx);
		pthread_mutex_init(&_q_done_mtx, nullptr);
		pthread_cond_destroy(&_q_done_cond);
		pthread_cond_init(&_q_done_cond, nullptr);
		_q_done = 0;
	}
	pthread_mutex_unlock(&_q_done_mtx);
	
	pthread_mutex_lock(&_bytes_mtx);
	{
		pthread_mutex_destroy(&_bytes_mtx);
		pthread_mutex_init(&_bytes_mtx, nullptr);
		_bytes_recvd = 0;
	}
	pthread_mutex_unlock(&_bytes_mtx);
	
	pthread_mutex_lock(&_file_mtx);
	{
		pthread_mutex_destroy(&_file_mtx);
		pthread_mutex_init(&_file_mtx, nullptr);
		_files_recvd = 0;
	}
	pthread_mutex_unlock(&_file_mtx);
}

void mirror_server::init() {
    struct sockaddr_in server;

    if ((_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket: ");
        exit(-1);
    }

    server.sin_family = AF_INET;
    server.sin_port = htons((uint16_t) _port);
    server.sin_addr.s_addr = INADDR_ANY;

    bzero(&server.sin_zero, 8);

    socklen_t len = sizeof(struct sockaddr_in);

    if ((bind(_sockfd, (sockaddr*) &server, len)) < 0) {
        perror("Bind");
        exit(-1);
    }


    if ((listen(_sockfd, 1)) < 0) {
        perror("Listen");
        exit(-1);
    }
}

void mirror_server::run() {
	// Workers will loop until done becomes true
	cerr << "Running" << endl;
    sockaddr_in client;
	do {
		
		pthread_mutex_lock(&_ack_mtx);
		{
			pthread_mutex_destroy(&_ack_mtx);
			pthread_mutex_init(&_ack_mtx, nullptr);
			pthread_cond_destroy(&_ack_cond);
			pthread_cond_init(&_ack_cond, nullptr);
			_ack = false;
		}
		pthread_mutex_unlock(&_ack_mtx);
		
	    socklen_t len = sizeof(struct sockaddr_in);
		int clientfd;
		cerr << "Listening to port " << _port << endl;
	    if ((clientfd = accept(_sockfd, (sockaddr *) &client, &len)) < 0) {
	        perror("accept");
	        exit(-1);
	    }
		g_clientfd = clientfd;

	    char *buffer = new char[1024];
	    ssize_t read = recv(clientfd, buffer, 1023, 0);
	    buffer[read] = '\0';

	    send(clientfd, "OK", 2, 0);
	    int vecs = atoi(buffer);

	    delete[] buffer;
	    my_vector<my_vector<my_string>> vec;
	    for (int i = 0; i < vecs; i++) {
	        my_vector<my_string> tmp;
	        for (int j = 0; j < 4; j++) {
	            buffer = new char[1024];
	            read = recv(clientfd, buffer, 1023, 0);
	            buffer[read] = '\0';
	            send(clientfd, "OK", 2, 0);
	            tmp.push(buffer);
	            delete[] buffer;

	        }
	        vec.push(tmp);
	    }

	    _managers = new pthread_t[vec.size()];
	    for (int i = 0; i < (int) vec.size(); i++) {
	        my_vector<my_string> tmp_vec = vec.at(i);
	        mirror_manager *man = new mirror_manager(tmp_vec, i, &_data_queue, 
				&_e_mtx, &_f_mtx, &_rw_mtx, &_e_cond, &_f_cond, 
				&_full, &_empty, _debug, _search);
	        pthread_attr_t attr;
	        pthread_attr_init(&attr);
	        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	        pthread_create(&_managers[i], &attr, start_manager, man);
	    }
	   
	    for (int count = 0; count < (int) vec.size(); count++) {
	        pthread_join(_managers[count], nullptr);
	    }
		
		cout << "DEBUG --::-- All mirrorManagers died" << endl;
		
		cout << "DEBUG --::-- Unblocking and joining worker threads!" << endl;
		// The boolean variables _empty and _done
		// share the same mutex
		pthread_mutex_lock(&_e_mtx);
		{
			_done = true;
			pthread_cond_broadcast(&_e_cond);
		}
		pthread_mutex_unlock(&_e_mtx);
		
		// Wait until a worker thread raises a queue done signal
		pthread_mutex_lock(&_q_done_mtx);
		{
			while(_q_done < _worker_num) {
				cerr << "Server waiting for signal!" << endl;
				pthread_cond_wait(&_q_done_cond, &_q_done_mtx);
				cerr << "Signal arrived and _q_done is " << _q_done << endl;
			}
			cerr << "Server got signal and is exiting" << endl;
		}
		pthread_mutex_unlock(&_q_done_mtx);

	    my_string msg = "OK:";
	    msg += _files_recvd;
	    msg += ":";
	    msg += _bytes_recvd;
		msg += ":";
		if (_files_recvd != 0)
			msg += (_bytes_recvd / _files_recvd);
		else 
			msg += 0;
		msg += ":";
		if (_files_recvd != 0)
			msg += ((int) sqrt(_bytes_recvd / _files_recvd));
		else 
			msg += 0;
	    msg += ";";
	    send(clientfd, msg.c_str(), msg.length(), 0);
	    close(clientfd);
		
		_mtx_init();
		
		pthread_mutex_lock(&_ack_mtx);
		{
			_ack = true;
			pthread_cond_broadcast(&_ack_cond);
		}
		pthread_mutex_unlock(&_ack_mtx);
		
		sleep(1);
		cout << "Session ended" << endl;
	} while (true);
}

void *start_manager(void *arg) {
    mirror_manager *man = (mirror_manager *) arg;
    if (!man->init()) {
		my_string msg = "ERR:";
		msg += man->get_addr();
		msg += ":";
		msg += man->get_port();
		msg += ";";
		
		send(g_clientfd, msg.c_str(), msg.length(), 0);
		hf::recv_ok(g_clientfd);
		
		delete man;
		pthread_exit(0);
	}
    man->run();
	cout << "Manager returned!!!!!" << endl;
    delete man;
    pthread_exit(0);
}

void *start_worker(void *arg) {
    worker *w = (worker *) arg;
    stats *statistics = w->run();
    delete w;
    pthread_exit(statistics);
}

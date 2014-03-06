// ----------------------------------------------------------------------------
//
//  Copyright (C) 2012 Fons Adriaensen <fons@linuxaudio.org>
//    
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------
#include <iostream>

#include <stdio.h>
#include <math.h>
#include <jack/thread.h>
#include "jackclient.h"
#include "alsathread.h"


Jackclient::Jackclient (jack_client_t* cl, const char*jserv, int mode, int nchan, void *arg) :
    _client (cl),
    _arg (arg),
    _mode (mode),
    _nchan (nchan),     
    _state (INIT),
    _freew (false)
{
    init (jserv);
}


Jackclient::~Jackclient (void)
{
    fini ();
}


void Jackclient::init (const char *jserv)
{
    int                 spol;
    jack_status_t       stat;
    struct sched_param  spar;

    if (_client == 0)
    {
        return;
    }
    jack_set_process_callback (_client, jack_static_process, (void *) this);
    jack_set_latency_callback (_client, jack_static_latency, (void *) this);
    jack_set_freewheel_callback (_client, jack_static_freewheel, (void *) this);
    jack_set_buffer_size_callback (_client, jack_static_buffsize, (void *) this);
    jack_on_shutdown (_client, jack_static_shutdown, (void *) this);

    _bsize = 0;
    _fsamp = 0;

    _jname = jack_get_client_name (_client);
    _bsize = jack_get_buffer_size (_client);
    _fsamp = jack_get_sample_rate (_client);

    if (_nchan) 
    {
       register_ports (_nchan);
    }

    _rprio = jack_client_real_time_priority (_client);
}

void Jackclient::register_ports (int n)
{
    int i, flags;
    char s [64];

    if (n > sizeof (_ports) / sizeof (_ports[0])) 
    {
        return;
    }

    flags = JackPortIsTerminal | JackPortIsPhysical;

    for (i = 0; i < n; i++)
    {
	if (_mode == PLAY)
	{
            sprintf (s, "playback_%d", i + 1);
            _ports [i] = jack_port_register (_client, s, JACK_DEFAULT_AUDIO_TYPE,
                                             flags | JackPortIsInput, 0);
	}
	else
	{
            sprintf (s, "capture_%d", i + 1);
            _ports [i] = jack_port_register (_client, s, JACK_DEFAULT_AUDIO_TYPE,
                                             flags | JackPortIsOutput, 0);
	}
    }

    _nchan = n;
    _buff = new float [_bsize * _nchan];
}

void Jackclient::fini (void)
{
    delete[] _buff;
}


void Jackclient::jack_static_shutdown (void *arg)
{
    ((Jackclient *) arg)->sendinfo (TERM, 0, 0);
}


int Jackclient::jack_static_buffsize (jack_nframes_t nframes, void *arg)
{
    Jackclient *J = (Jackclient *) arg;

    if (J->_bsize == 0)	J->_bsize = nframes;
    else if (J->_bsize != (int) nframes) J->_state = Jackclient::TERM;
    return 0;
}


void Jackclient::jack_static_freewheel (int state, void *arg)
{
    ((Jackclient *) arg)->jack_freewheel (state);
}


void Jackclient::jack_static_latency (jack_latency_callback_mode_t jlcm, void *arg)
{
    ((Jackclient *) arg)->jack_latency (jlcm);
}


int Jackclient::jack_static_process (jack_nframes_t nframes, void *arg)
{
    return ((Jackclient *) arg)->jack_process (nframes);
}


void Jackclient::start (Lfq_audio   *audioq,
                        Lfq_int32   *commq, 
                        Lfq_adata   *alsaq,
                        Lfq_jdata   *infoq,
                        double      ratio,
                        int         delay,
			int         ltcor,
                        int         rqual)
{
    double d;

    _audioq = audioq;
    _commq = commq;
    _alsaq = alsaq;
    _infoq = infoq;
    _quant = ldexp (1e-6f, 28);
    _ratio = ratio;
    _rcorr = 1.0;
    _resamp.setup (_ratio, _nchan, rqual);
    _resamp.set_rrfilt (100);
    d = _resamp.inpsize () / 2.0;
    if (_mode == PLAY) d *= _ratio;
    _delay = delay + d;
    _ltcor = ltcor;
    _ppsec = (_fsamp + _bsize / 2) / _bsize;

    if (jack_activate (_client))
    {
        fprintf(stderr, "Can't activate Jack");
        return;
    }

    initwait (_ppsec / 2);

    jack_recompute_total_latencies (_client);
}


void Jackclient::initwait (int nwait)
{
    _count = -nwait;
    _commq->wr_int32 (Alsathread::WAIT);
    _state = WAIT;
    if (nwait > _ppsec) sendinfo (WAIT, 0, 0);
}


void Jackclient::initsync (void)
{
    // Reset all lock-free queues.
    _commq->reset ();
    _alsaq->reset ();
    _audioq->reset ();
    // Reset and prefill the resampler.
    _resamp.reset ();
    _resamp.inp_count = _resamp.inpsize () / 2 - 1;
    _resamp.out_count = 10000;
    _resamp.process ();
    // Initiliase state variables.
    _t_a0 = _t_a1 = 0;
    _k_a0 = _k_a1 = 0;
    _k_j0 = 0;
    // Initialise loop filter state.
    _z1 = _z2 = _z3 = 0;
    // Activate the ALSA thread,
    _commq->wr_int32 (Alsathread::PROC);
    _state = SYNC0;
    sendinfo (SYNC0, 0, 0);
}


void Jackclient::setloop (double bw)
{
    double w;

    // Set the loop bandwidth to bw Hz.
    w = 6.28f * 20 * bw * _bsize / _fsamp;
    _w0 = 1.0 - exp (-w);
    w = 6.28f * bw * _ratio / _fsamp;
    _w1 = w * 1.6;
    _w2 = w * _bsize / 1.6;
}


void Jackclient::playback (int nframes)
{
    int    i, j, n;
    float  *p, *q;

    // Interleave inputs into _buff.
    for (i = 0; i < _nchan; i++)
    {
	p = (float *)(jack_port_get_buffer (_ports [i], nframes));
	q = _buff + i;
	for (j = 0; j < _bsize; j++) q [j * _nchan] = p [j];
    }       
    // Resample _buff and write to audio queue.
    // The while loop takes care of wraparound.
    _resamp.inp_count = _bsize;
    _resamp.inp_data  = _buff;
    while (_resamp.inp_count)
    {
	_resamp.out_count = _audioq->wr_linav ();
	_resamp.out_data  = _audioq->wr_datap ();
	n = _resamp.out_count;
	_resamp.process ();
	n -= _resamp.out_count;
	_audioq->wr_commit (n);
	// Adjust state by the number of frames used.
	_k_j0 += n;
    }
}


void Jackclient::capture (int nframes)
{
    int    i, j, n;
    float  *p, *q;

    // Read from audio queue and resample.
    // The while loop takes care of wraparound.
    _resamp.out_count = _bsize;
    _resamp.out_data  = _buff;
    while (_resamp.out_count)
    {
	_resamp.inp_count = _audioq->rd_linav ();
	_resamp.inp_data  = _audioq->rd_datap ();
	n = _resamp.inp_count;
	_resamp.process ();
	n -= _resamp.inp_count;
	_audioq->rd_commit (n);
	// Adjust state by the number of frames used.
	_k_j0 += n;
    }
    // Deinterleave _buff to outputs.
    for (i = 0; i < _nchan; i++)
    {
	p = _buff + i;
	q = (float *)(jack_port_get_buffer (_ports [i], nframes));
	for (j = 0; j < _bsize; j++) q [j] = p [j * _nchan];
    }       
}


void Jackclient::silence (int nframes)
{
    int    i;
    float  *q;

    // Write silence to all jack ports.
    for (i = 0; i < _nchan; i++)
    {
        q = (float *)(jack_port_get_buffer (_ports [i], nframes));
        memset (q, 0, nframes * sizeof (float));
    }
}


void Jackclient::sendinfo (int state, double error, double ratio)
{
    Jdata *J;

    if (_infoq->wr_avail ())
    {
	J = _infoq->wr_datap ();
	J->_state = state;
	J->_error = error;
	J->_ratio = ratio;
	_infoq->wr_commit ();
    }
}


void Jackclient::jack_freewheel (int state)
{
    _freew = state ? true : false;
    if (_freew) initwait (_ppsec / 4);
}


void Jackclient::jack_latency (jack_latency_callback_mode_t jlcm)
{
    jack_latency_range_t R;
    int i;

    if (_state < WAIT) return;
    if (_mode == PLAY)
    {
	if (jlcm != JackPlaybackLatency) return;
	R.min = R.max = (int)(_delay / _ratio) + _ltcor;
    }
    else
    {
	if (jlcm != JackCaptureLatency) return;
	R.min = R.max = (int)(_delay * _ratio) + _ltcor;
    }
    for (i = 0; i < _nchan; i++)
    {
	jack_port_set_latency_range (_ports [i], jlcm, &R);
    }
}


int Jackclient::jack_process (int nframes)
{
    int             dk, n;
    Adata           *D;
    jack_nframes_t  ft;
    double          tj, err, d1, d2;

    // Buffer size change or other evil.
    if (_state == TERM)
    {
	sendinfo (TERM, 0, 0);
	return 0;
    }
    // Skip cylce if ports may not yet exist.
    if (_state < WAIT) return 0;

    // Start synchronisation 1/2 second after entering
    // the WAIT state. This delay allows the ALSA thread
    // to restart cleanly if necessary. Disabled while
    // freewheeling.
    if (_state == WAIT)
    {
	if (_freew) return 0;
	if (_mode == CAPT) silence (nframes);
        if (++_count == 0) initsync ();
        else return 0;
    }

    // Compute the start time of the current cycle.
    // Jack should really provide the usecs directly.
    ft = jack_last_frame_time (_client);
    tj = 1e-6 * (jack_frames_to_time (_client, ft) & 0x0FFFFFFF);

    // Check for any skipped cycles. This is invalid
    // the first time, but won't be used then anyway.
    dk = ft - _ft - _bsize;
    _ft = ft;

    // Check if we have timing data from the ALSA thread.
    n = _alsaq->rd_avail ();
    // If the data queue is full restart synchronisation.
    // This can happen e.g. on a jack engine timeout, or
    // when too many cycles have been skipped.
    if (n == _alsaq->size ())
    {
        initwait (_ppsec / 2);
        return 0;
    }
    if (n)
    {
        // Else move interval end to start, and update the
        // interval end keeping only the most recent data.
        if (_state < SYNC2) _state++;
        _t_a0 = _t_a1;
        _k_a0 = _k_a1;
        while (_alsaq->rd_avail ())
        {
            D = _alsaq->rd_datap ();
            // Restart synchronisation in case of
            // an error in the ALSA interface.
            if (D->_state == Alsathread::WAIT)
            {
                initwait (_ppsec / 2);
                return 0;
            }
            _t_a1 =  D->_timer;
            _k_a1 += D->_nsamp;
            _alsaq->rd_commit ();
        }
    }

    err = 0;
    if (_state >= SYNC2)
    {
        // Compute the delay error.
        d1 = modtime (tj - _t_a0);
        d2 = modtime (_t_a1 - _t_a0);
	if (_mode == PLAY)
	{
            n = _k_j0 - _k_a0; // Must be done as integer as both terms will overflow.
            err = n - (_k_a1 - _k_a0) * d1 / d2  + _resamp.inpdist () * _ratio - _delay;
	}
	else
	{
            n = _k_a0 - _k_j0;
            err = n + (_k_a1 - _k_a0) * d1 / d2  + _resamp.inpdist () - _delay ;
	}

        n = (int)(floor (err + 0.5));
        if (_state == SYNC2)
        {
            // We have the first delay error value. Adjust both the state
            // variables and the audio queue by the same number of frames
            // to obtain the actually wanted delay, and start tracking.
	    if (_mode == PLAY)
	    {
                _audioq->wr_commit (-n);
                _k_j0 -= n;
	    }
	    else
	    {
                _audioq->rd_commit (n);
                _k_j0 += n;
	    }
            err -= n;
            setloop (1.0);
            _state = PROC1;
        }    
        else if (_state >= PROC1)
        {
	    // Check error conditions.
	    if (dk)
	    {
                // Jack skipped some cycles. Adjust both the state
                // and the audio queue so we can just continue.
		// The loop will correct the remaining fraction
		// of a frame.
	        if (_mode == PLAY)
		{
		    dk = (int)(dk * _ratio + 0.5);
                    if (abs (dk + n) < _bsize / 4)
                    {
                        _audioq->wr_commit (dk);
                        _k_j0 += dk;
                        err += dk;
		        n = 0;
		    }
		}
		else
		{
		    dk = (int)(dk / _ratio + 0.5);    
                    if (abs (dk - n) < _bsize / 4)
		    {                
                        _audioq->rd_commit (dk);
                        _k_j0 += dk;
                        err -= dk;
		        n = 0;
		    }
		}
            }
            if (fabs (err) >= 50)
            {
                // Something is really wrong, wait 15 seconds then restart.
                initwait (15 * _ppsec);
		return 0;
            }
        }
    }

    // Switch to lower bandwidth after 4 seconds.
    if ((_state == PROC1) && (++_count == 4 * _ppsec))
    {
        _state = PROC2;
        setloop (0.05);
    } 

    if (_state >= PROC1)
    {
        // Run loop filter and set resample ratio.
        _z1 += _w0 * (_w1 * err - _z1);
        _z2 += _w0 * (_z1 - _z2);
        _z3 += _w2 * _z2;
        _rcorr = 1 - _z2 - _z3;
	if (_rcorr > 1.05) _rcorr = 1.05;
	if (_rcorr < 0.95) _rcorr = 0.95;
        _resamp.set_rratio (_rcorr);
	sendinfo (_state, err, _rcorr);

	// Resample and transfer between audio
        // queue and jack ports.
	if (_mode == PLAY) playback (nframes);
	else               capture (nframes);
    }
    else if (_mode == CAPT) silence (nframes);

    return 0; 
}

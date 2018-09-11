/*
 * display.hpp
 *
 *  Created on: Sep 11, 2018
 *      Author: dwd
 */

#pragma once

#include "../../vp-display/framebuffer.h"
#include <systemc>
#include <tlm_utils/simple_target_socket.h>

using namespace std;
using namespace sc_core;
using namespace tlm_utils;

struct Display : public sc_core::sc_module
{
	static const size_t addressRange = sizeof(Framebuffer);

	simple_target_socket<Display> tsock;

	union
	{
		uint8_t* raw;
		Framebuffer* buf;
	} frame;

	void createSM();

	Display(sc_module_name);
	void transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);
};

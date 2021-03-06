/*
 *
 * (C) 2013-20 - ntop.org
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "ntop_includes.h"

#ifndef HAVE_NEDGE

extern "C" {
#include "../third-party/snmp/snmp.c"
#include "../third-party/snmp/asn1.c"
#include "../third-party/snmp/net.c"
};

/* ******************************* */

SNMP::SNMP() {
  char version[4] = { '\0' };

  ntop->getRedis()->get((char*)CONST_RUNTIME_PREFS_SNMP_PROTO_VERSION, version, sizeof(version));

  if((udp_sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    throw("Unable to start network discovery");

  Utils::maximizeSocketBuffer(udp_sock, true /* RX */, 2 /* MB */);
  snmp_version = atoi(version);
  if(snmp_version > 1 /* v2c */) snmp_version = 1;
}

/* ******************************* */

SNMP::~SNMP() {
  if(udp_sock != -1) closesocket(udp_sock);
}

/* ******************************************* */

void SNMP::send_snmp_request(char *agent_host, char *community, bool isGetNext,
			     char *oid[SNMP_MAX_NUM_OIDS], u_int version) {
  u_int agent_port = 161, request_id = (u_int)time(NULL);
  int i = 0;
  SNMPMessage *message;
  int len;
  u_char buf[1500];
  int operation = isGetNext ? SNMP_GETNEXT_REQUEST_TYPE : SNMP_GET_REQUEST_TYPE;

  if((message = snmp_create_message())) {
    snmp_set_version(message, version);
    snmp_set_community(message, community);
    snmp_set_pdu_type(message, operation);
    snmp_set_request_id(message, request_id);
    snmp_set_error(message, 0);
    snmp_set_error_index(message, 0);

    for(i=0; i<SNMP_MAX_NUM_OIDS; i++) {
      if(oid[i] != NULL)
	snmp_add_varbind_null(message, oid[i]);
    }

    len = snmp_message_length(message);
    snmp_render_message(message, buf);
    snmp_destroy_message(message);
    free(message); /* malloc'd by snmp_create_message */

    send_udp_datagram(buf, len, udp_sock, agent_host, agent_port);
  }
}

/* ******************************************* */

int SNMP::snmp_read_response(lua_State* vm, u_int timeout) {
  int i = 0;

  if(ntop->getGlobals()->isShutdown()
     || input_timeout(udp_sock, timeout) == 0) {
    /* Timeout or shutdown in progress */
    lua_pushnil(vm);
  } else {
    char buf[BUFLEN];
    SNMPMessage *message;
    char *sender_host, *oid_str,  *value_str;
    int sender_port, added = 0, len;

    /* This receive doesn't block */
    len = receive_udp_datagram(buf, BUFLEN, udp_sock, &sender_host, &sender_port);
    message = snmp_parse_message(buf, len);

    i = 0;
    while(snmp_get_varbind_as_string(message, i, &oid_str, NULL, &value_str)) {
      if(!added) lua_newtable(vm), added = 1;
      lua_push_str_table_entry(vm, oid_str, value_str);
      if(value_str) free(value_str), value_str = NULL;
      i++;
    }

    snmp_destroy_message(message);
    free(message); /* malloc'd by snmp_parse_message */

    if(!added)
      lua_pushnil(vm);
  }

  return(CONST_LUA_OK);
}

/* ******************************************* */

int SNMP::snmp_get_fctn(lua_State* vm, bool isGetNext, bool skip_first_param) {
  char *agent_host, *community;
  u_int timeout = 5, version = snmp_version, oid_idx = 0, idx = skip_first_param ? 2 : 1;
  char *oid[SNMP_MAX_NUM_OIDS] = { NULL };

  if(ntop_lua_check(vm, __FUNCTION__, idx, LUA_TSTRING) != CONST_LUA_OK)  return(CONST_LUA_ERROR);
  agent_host = (char*)lua_tostring(vm, idx++);

  if(ntop_lua_check(vm, __FUNCTION__, idx, LUA_TSTRING) != CONST_LUA_OK)  return(CONST_LUA_ERROR);
  community = (char*)lua_tostring(vm, idx++);

  if(ntop_lua_check(vm, __FUNCTION__, idx, LUA_TSTRING) != CONST_LUA_OK)  return(CONST_LUA_ERROR);
  oid[oid_idx++] = (char*)lua_tostring(vm, idx++);

  /* Optional timeout: take the minimum */
  if(lua_type(vm, idx) == LUA_TNUMBER) {
    timeout = min(timeout, (u_int)lua_tointeger(vm, idx));
    idx++;
  }

  /* Optional version */
  if(lua_type(vm, idx) == LUA_TNUMBER) {
    version = (u_int)lua_tointeger(vm, idx);
    idx++;
  }

  /* Add additional OIDs */
  while((oid_idx < SNMP_MAX_NUM_OIDS) && (lua_type(vm, idx) == LUA_TSTRING)) {
    oid[oid_idx++] = (char*)lua_tostring(vm, idx);
    idx++;
  }

  send_snmp_request(agent_host, community, isGetNext, oid, version);

  if(skip_first_param)
    return(CONST_LUA_OK); /* This is an async call */
  else
    return(snmp_read_response(vm, timeout));
}

/* ******************************************* */

int SNMP::get(lua_State* vm, bool skip_first_param) {
  return(snmp_get_fctn(vm, false, skip_first_param));
}

/* ******************************************* */

int SNMP::getnext(lua_State* vm, bool skip_first_param) {
  return(snmp_get_fctn(vm, true, skip_first_param));
}

/* ******************************************* */

void SNMP::snmp_fetch_responses(lua_State* vm, u_int sec_timeout, bool add_sender_ip) {
  int i = 0;

  if(ntop->getGlobals()->isShutdown()
     || input_timeout(udp_sock, sec_timeout) == 0) {
    /* Timeout or shutdown in progress */
  } else {
    char buf[BUFLEN];
    SNMPMessage *message;
    char *sender_host, *oid_str, *value_str = NULL;
    int sender_port, len;

    len = receive_udp_datagram(buf, BUFLEN, udp_sock, &sender_host, &sender_port);

    if((message = snmp_parse_message(buf, len))) {
      bool table_added = false;
      
      i = 0;

      while(snmp_get_varbind_as_string(message, i, &oid_str, NULL, &value_str)) {
	if(value_str /* && (value_str[0] != '\0') */) {
	  if(!table_added)
	    lua_newtable(vm), table_added = true;
	  
	  if(add_sender_ip /* Used in batch mode */) {
	    /*
	      The key is the IP address as this is used when contacting multiple
	      hosts so we need to know who has sent back the response
	    */
	    lua_push_str_table_entry(vm, sender_host /* Sender IP */, value_str);
	  } else
	    lua_push_str_table_entry(vm, oid_str, value_str);
	  
	  free(value_str), value_str = NULL; /* malloc'd by snmp_get_varbind_as_string */
	}

	i++;
      } /* while */

      snmp_destroy_message(message);
      free(message); /* malloc'd by snmp_parse_message */
      if(table_added)
	return;
    }
  }

  lua_pushnil(vm);
}

#endif /* HAVE_NEDGE */

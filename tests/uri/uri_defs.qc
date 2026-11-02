/* the builtins the uri tests use beyond ../bots/defs.qc */
float(string url, float id, optional string mimetype, optional string body) uri_get = #0:uri_get;
float(string name) addprogs = #0:addprogs;
float(string s) strlen = #0:strlen;
float(string s, string sub, optional float start) strstrofs = #0:strstrofs;
void(entity e, string key, string value) forceinfokey;

float g_got;
void uri_line(string who, float id, float code, string body)
{
	print(sprintf("URI %s id %d code %d body %s\n", who, id, code, body));
}

/* entry points neither progs uses */
void main(void) { }
void PlayerPreThink(void) { }
void PlayerPostThink(void) { }
void ClientKill(void) { }
void ClientConnect(void) { }
void PutClientInServer(void) { }
void ClientDisconnect(void) { }
void SetNewParms(void) { }
void SetChangeParms(void) { }

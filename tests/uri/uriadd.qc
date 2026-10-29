/* the addprogs half of uritest.qc */
void init(float prevprogs)
{
	uri_get(strcat(cvar_string("uritest_base"), "/v1/database/csnz/sql"), 4, "text/plain", "SELECT 1");
}

void URI_Get_Callback(float id, float code, string body, float len)
{
	uri_line("add", id, code, body);
}

void StartFrame(void) { }

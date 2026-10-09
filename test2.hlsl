float evalF(float x, float y) { return x + y + i; }
float4 PSMain(float4 pos : SV_POSITION) : SV_Target { return evalF(pos.x, pos.y); }

/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2016      Spike

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"
#include "q_ctype.h"

//there's a few different aproaches to tempstrings...
//the lame way is to just have a single one (vanilla).
//the only slightly less lame way is to just cycle between 16 or so (most engines).
//one funky way is to allocate a single large buffer and just concatenate it for more tempstring space. don't forget to resize (dp).
//alternatively, just allocate them persistently and purge them only when there appear to be no more references to it (fte). makes strzone redundant.

cvar_t pr_checkextension = {"pr_checkextension", "1", CVAR_NONE};	//spike - enables qc extensions. if 0 then they're ALL BLOCKED! MWAHAHAHA! *cough* *splutter*
struct pr_extfuncs_s pr_extfuncs;

void SV_CheckVelocity (edict_t *ent);

typedef enum multicast_e
{
	MULTICAST_ALL_U,
	MULTICAST_PHS_U,
	MULTICAST_PVS_U,
	MULTICAST_ALL_R,
	MULTICAST_PHS_R,
	MULTICAST_PVS_R,

	MULTICAST_ONE_U,
	MULTICAST_ONE_R,
	MULTICAST_INIT
} multicast_t;
static void SV_Multicast(multicast_t to, float *org, int msg_entity, unsigned int requireext2);

#define Z_StrDup(s) strcpy(Z_Malloc(strlen(s)+1), s)
#define	RETURN_EDICT(e) (((int *)pr_globals)[OFS_RETURN] = EDICT_TO_PROG(e))

//provides a few convienience extensions, primarily builtins, but also autocvars.
//Also note the set+seta features.

#define D(typestr,desc) typestr,desc

//#define fixme

//maths stuff
static void PF_Sin(void)
{
	G_FLOAT(OFS_RETURN) = sin(G_FLOAT(OFS_PARM0));
}
static void PF_asin(void)
{
	G_FLOAT(OFS_RETURN) = asin(G_FLOAT(OFS_PARM0));
}
static void PF_Cos(void)
{
	G_FLOAT(OFS_RETURN) = cos(G_FLOAT(OFS_PARM0));
}
static void PF_acos(void)
{
	G_FLOAT(OFS_RETURN) = acos(G_FLOAT(OFS_PARM0));
}
static void PF_tan(void)
{
	G_FLOAT(OFS_RETURN) = tan(G_FLOAT(OFS_PARM0));
}
static void PF_atan(void)
{
	G_FLOAT(OFS_RETURN) = atan(G_FLOAT(OFS_PARM0));
}
static void PF_atan2(void)
{
	G_FLOAT(OFS_RETURN) = atan2(G_FLOAT(OFS_PARM0), G_FLOAT(OFS_PARM1));
}
static void PF_Sqrt(void)
{
	G_FLOAT(OFS_RETURN) = sqrt(G_FLOAT(OFS_PARM0));
}
static void PF_pow(void)
{
	G_FLOAT(OFS_RETURN) = pow(G_FLOAT(OFS_PARM0), G_FLOAT(OFS_PARM1));
}
static void PF_Logarithm(void)
{
	//log2(v) = ln(v)/ln(2)
	double r;
	r = log(G_FLOAT(OFS_PARM0));
	if (pr_argc > 1)
		r /= log(G_FLOAT(OFS_PARM1));
	G_FLOAT(OFS_RETURN) = r;
}
static void PF_mod(void)
{
	float a = G_FLOAT(OFS_PARM0);
	float n = G_FLOAT(OFS_PARM1);

	if (n == 0)
	{
		Con_DWarning("PF_mod: mod by zero\n");
		G_FLOAT(OFS_RETURN) = 0;
	}
	else
	{
		//because QC is inherantly floaty, lets use floats.
		G_FLOAT(OFS_RETURN) = a - (n * (int)(a/n));
	}
}
static void PF_min(void)
{
	float r = G_FLOAT(OFS_PARM0);
	int i;
	for (i = 1; i < pr_argc; i++)
	{
		if (r > G_FLOAT(OFS_PARM0 + i*3))
			r = G_FLOAT(OFS_PARM0 + i*3);
	}
	G_FLOAT(OFS_RETURN) = r;
}
static void PF_max(void)
{
	float r = G_FLOAT(OFS_PARM0);
	int i;
	for (i = 1; i < pr_argc; i++)
	{
		if (r < G_FLOAT(OFS_PARM0 + i*3))
			r = G_FLOAT(OFS_PARM0 + i*3);
	}
	G_FLOAT(OFS_RETURN) = r;
}
static void PF_bound(void)
{
	float minval = G_FLOAT(OFS_PARM0);
	float curval = G_FLOAT(OFS_PARM1);
	float maxval = G_FLOAT(OFS_PARM2);
	if (curval > maxval)
		curval = maxval;
	if (curval < minval)
		curval = minval;
	G_FLOAT(OFS_RETURN) = curval;
}
static void PF_anglemod(void)
{
	float v = G_FLOAT(OFS_PARM0);

	while (v >= 360)
		v = v - 360;
	while (v < 0)
		v = v + 360;

	G_FLOAT(OFS_RETURN) = v;
}
static void PF_bitshift(void)
{
	int bitmask = G_FLOAT(OFS_PARM0);
	int shift = G_FLOAT(OFS_PARM1);
	if (shift < 0)
		bitmask >>= -shift;
	else
		bitmask <<= shift;
	G_FLOAT(OFS_RETURN) = bitmask;
}
static void PF_crossproduct(void)
{
	CrossProduct(G_VECTOR(OFS_PARM0), G_VECTOR(OFS_PARM1), G_VECTOR(OFS_RETURN));
}
static void PF_vectorvectors(void)
{
	VectorCopy(G_VECTOR(OFS_PARM0), pr_global_struct->v_forward);
	VectorNormalize(pr_global_struct->v_forward);
	if (!pr_global_struct->v_forward[0] && !pr_global_struct->v_forward[1])
	{
		if (pr_global_struct->v_forward[2])
			pr_global_struct->v_right[1] = -1;
		else
			pr_global_struct->v_right[1] = 0;
		pr_global_struct->v_right[0] = pr_global_struct->v_right[2] = 0;
	}
	else
	{
		pr_global_struct->v_right[0] = pr_global_struct->v_forward[1];
		pr_global_struct->v_right[1] = -pr_global_struct->v_forward[0];
		pr_global_struct->v_right[2] = 0;
		VectorNormalize(pr_global_struct->v_right);
	}
	CrossProduct(pr_global_struct->v_right, pr_global_struct->v_forward, pr_global_struct->v_up);
}
static void PF_ext_vectoangles(void)
{	//alternative version of the original builtin, that can deal with roll angles too, by accepting an optional second argument for 'up'.
	float	*value1, *up;

	value1 = G_VECTOR(OFS_PARM0);
	if (pr_argc >= 2)
		up = G_VECTOR(OFS_PARM1);
	else
		up = NULL;

	VectorAngles(value1, up, G_VECTOR(OFS_RETURN));
	G_VECTOR(OFS_RETURN)[PITCH] *= -1;	//this builtin is for use with models. models have an inverted pitch. consistency with makevectors would never do!
}

//string stuff
static void PF_strlen(void)
{	//FIXME: doesn't try to handle utf-8
	const char *s = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = strlen(s);
}
static void PF_strcat(void)
{
	int		i;
	char *out = PR_GetTempString();
	size_t s;

	out[0] = 0;
	s = 0;
	for (i = 0; i < pr_argc; i++)
	{
		s = q_strlcat(out, G_STRING((OFS_PARM0+i*3)), STRINGTEMP_LENGTH);
		if (s >= STRINGTEMP_LENGTH)
		{
			Con_Warning("PF_strcat: overflow (string truncated)\n");
			break;
		}
	}

	G_INT(OFS_RETURN) = PR_SetEngineString(out);
}
static void PF_substring(void)
{
	int start, length, slen;
	const char *s;
	char *string;

	s = G_STRING(OFS_PARM0);
	start = G_FLOAT(OFS_PARM1);
	length = G_FLOAT(OFS_PARM2);

	slen = strlen(s);	//utf-8 should use chars, not bytes.

	if (start < 0)
		start = slen+start;
	if (length < 0)
		length = slen-start+(length+1);
	if (start < 0)
	{
	//	length += start;
		start = 0;
	}

	if (start >= slen || length<=0)
	{
		G_INT(OFS_RETURN) = PR_SetEngineString("");
		return;
	}

	slen -= start;
	if (length > slen)
		length = slen;
	//utf-8 should switch to bytes now.
	s += start;

	if (length >= STRINGTEMP_LENGTH)
	{
		length = STRINGTEMP_LENGTH-1;
		Con_Warning("PF_substring: truncation\n");
	}

	string = PR_GetTempString();
	memcpy(string, s, length);
	string[length] = '\0';
	G_INT(OFS_RETURN) = PR_SetEngineString(string);
}
/*our zoned strings implementation is somewhat specific to quakespasm, so good luck porting*/
unsigned char *knownzone;
size_t knownzonesize;
static void PF_strzone(void)
{
	char *buf;
	size_t len = 0;
	const char *s[8];
	size_t l[8];
	int i;
	size_t id;
	for (i = 0; i < pr_argc; i++)
	{
		s[i] = G_STRING(OFS_PARM0+i*3);
		l[i] = strlen(s[i]);
		len += l[i];
	}
	len++; /*for the null*/

	buf = Z_Malloc(sizeof(int));
	G_INT(OFS_RETURN) = PR_SetEngineString(buf);
	id = -1-G_INT(OFS_RETURN);
	if (id <= knownzonesize)
	{
		knownzonesize = (id+32)&~7;
		knownzone = Z_Realloc(knownzone, knownzonesize>>3);
	}
	knownzone[id>>3] |= 1u<<(id&7);
	
	len = 0;
	for (i = 0; i < pr_argc; i++)
	{
		memcpy(buf, s[i], l[i]);
		buf += l[i];
	}
	*buf = '\0';
}
static void PF_strunzone(void)
{
	size_t id;
	const char *foo = G_STRING(OFS_PARM0);
	if (!G_INT(OFS_PARM0))
		return;	//don't bug out if they gave a dodgy string
	id = -1-G_INT(OFS_RETURN);
	if (id < knownzonesize && (knownzone[id>>3] & (1u<<(id&7))))
	{
		knownzone[id>>3] &= ~(1u<<(id&7));
		Z_Free((void*)foo);
		PR_ClearEngineString(G_INT(OFS_PARM0));
	}
	else
		Con_Warning("PF_strunzone: string wasn't strzoned\n");
}
static void PR_UnzoneAll(void)
{	//called to clean up all zoned strings.
	while (knownzonesize --> 0)
	{
		size_t id = knownzonesize;
		string_t s = -1-(int)id;
		if (knownzone[id>>3] & (1u<<(id&7)))
		{
			char *ptr = (char*)PR_GetString(s);
			PR_ClearEngineString(s);
			Z_Free(ptr);
		}
	}
	if (knownzone)
		Z_Free(knownzone);
	knownzonesize = 0;
	knownzone = NULL;
}
static void PF_str2chr(void)
{
	const char *instr = G_STRING(OFS_PARM0);
	int ofs = (pr_argc>1)?G_FLOAT(OFS_PARM1):0;

	if (ofs < 0)
		ofs = strlen(instr)+ofs;

	if (ofs && (ofs < 0 || ofs > (int)strlen(instr)))
		G_FLOAT(OFS_RETURN) = '\0';
	else
		G_FLOAT(OFS_RETURN) = (unsigned char)instr[ofs];
}
static void PF_chr2str(void)
{
	char *ret = PR_GetTempString(), *out;
	int i;
	for (i = 0, out=ret; i < pr_argc; i++)
		*out++ = G_FLOAT(OFS_PARM0 + i*3);
	*out = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(ret);
}
//part of PF_strconv
static int chrconv_number(int i, int base, int conv)
{
	i -= base;
	switch (conv)
	{
	default:
	case 5:
	case 6:
	case 0:
		break;
	case 1:
		base = '0';
		break;
	case 2:
		base = '0'+128;
		break;
	case 3:
		base = '0'-30;
		break;
	case 4:
		base = '0'+128-30;
		break;
	}
	return i + base;
}
//part of PF_strconv
static int chrconv_punct(int i, int base, int conv)
{
	i -= base;
	switch (conv)
	{
	default:
	case 0:
		break;
	case 1:
		base = 0;
		break;
	case 2:
		base = 128;
		break;
	}
	return i + base;
}
//part of PF_strconv
static int chrchar_alpha(int i, int basec, int baset, int convc, int convt, int charnum)
{
	//convert case and colour seperatly...

	i -= baset + basec;
	switch (convt)
	{
	default:
	case 0:
		break;
	case 1:
		baset = 0;
		break;
	case 2:
		baset = 128;
		break;

	case 5:
	case 6:
		baset = 128*((charnum&1) == (convt-5));
		break;
	}

	switch (convc)
	{
	default:
	case 0:
		break;
	case 1:
		basec = 'a';
		break;
	case 2:
		basec = 'A';
		break;
	}
	return i + basec + baset;
}
//FTE_STRINGS
//bulk convert a string. change case or colouring.
static void PF_strconv (void)
{
	int ccase = G_FLOAT(OFS_PARM0);		//0 same, 1 lower, 2 upper
	int redalpha = G_FLOAT(OFS_PARM1);	//0 same, 1 white, 2 red,  5 alternate, 6 alternate-alternate
	int rednum = G_FLOAT(OFS_PARM2);	//0 same, 1 white, 2 red, 3 redspecial, 4 whitespecial, 5 alternate, 6 alternate-alternate
	const unsigned char *string = (const unsigned char*)PF_VarString(3);
	int len = strlen((const char*)string);
	int i;
	unsigned char *resbuf = (unsigned char*)PR_GetTempString();
	unsigned char *result = resbuf;

	//UTF-8-FIXME: cope with utf+^U etc

	if (len >= STRINGTEMP_LENGTH)
		len = STRINGTEMP_LENGTH-1;

	for (i = 0; i < len; i++, string++, result++)	//should this be done backwards?
	{
		if (*string >= '0' && *string <= '9')	//normal numbers...
			*result = chrconv_number(*string, '0', rednum);
		else if (*string >= '0'+128 && *string <= '9'+128)
			*result = chrconv_number(*string, '0'+128, rednum);
		else if (*string >= '0'+128-30 && *string <= '9'+128-30)
			*result = chrconv_number(*string, '0'+128-30, rednum);
		else if (*string >= '0'-30 && *string <= '9'-30)
			*result = chrconv_number(*string, '0'-30, rednum);

		else if (*string >= 'a' && *string <= 'z')	//normal numbers...
			*result = chrchar_alpha(*string, 'a', 0, ccase, redalpha, i);
		else if (*string >= 'A' && *string <= 'Z')	//normal numbers...
			*result = chrchar_alpha(*string, 'A', 0, ccase, redalpha, i);
		else if (*string >= 'a'+128 && *string <= 'z'+128)	//normal numbers...
			*result = chrchar_alpha(*string, 'a', 128, ccase, redalpha, i);
		else if (*string >= 'A'+128 && *string <= 'Z'+128)	//normal numbers...
			*result = chrchar_alpha(*string, 'A', 128, ccase, redalpha, i);

		else if ((*string & 127) < 16 || !redalpha)	//special chars..
			*result = *string;
		else if (*string < 128)
			*result = chrconv_punct(*string, 0, redalpha);
		else
			*result = chrconv_punct(*string, 128, redalpha);
	}
	*result = '\0';

	G_INT(OFS_RETURN) = PR_SetEngineString((char*)resbuf);
}
static void PF_strpad(void)
{
	char *destbuf = PR_GetTempString();
	char *dest = destbuf;
	int pad = G_FLOAT(OFS_PARM0);
	const char *src = PF_VarString(1);

	//UTF-8-FIXME: pad is chars not bytes...

	if (pad < 0)
	{	//pad left
		pad = -pad - strlen(src);
		if (pad>=STRINGTEMP_LENGTH)
			pad = STRINGTEMP_LENGTH-1;
		if (pad < 0)
			pad = 0;

		q_strlcpy(dest+pad, src, STRINGTEMP_LENGTH-pad);
		while(pad)
		{
			dest[--pad] = ' ';
		}
	}
	else
	{	//pad right
		if (pad>=STRINGTEMP_LENGTH)
			pad = STRINGTEMP_LENGTH-1;
		pad -= strlen(src);
		if (pad < 0)
			pad = 0;

		q_strlcpy(dest, src, STRINGTEMP_LENGTH);
		dest+=strlen(dest);

		while(pad-->0)
			*dest++ = ' ';
		*dest = '\0';
	}

	G_INT(OFS_RETURN) = PR_SetEngineString(destbuf);
}
static void PF_infoadd(void)
{
	const char *info = G_STRING(OFS_PARM0);
	const char *key = G_STRING(OFS_PARM1);
	const char *value = PF_VarString(2);
	char *destbuf = PR_GetTempString(), *o = destbuf, *e = destbuf + STRINGTEMP_LENGTH - 1;

	size_t keylen = strlen(key);
	size_t valuelen = strlen(value);
	if (!*key)
	{	//error
		G_INT(OFS_RETURN) = G_INT(OFS_PARM0);
		return;
	}

	//copy the string to the output, stripping the named key
	while(*info)
	{
		const char *l = info;
		if (*info++ != '\\')
			break;	//error / end-of-string

		if (!strncmp(info, key, keylen) && info[keylen] == '\\')
		{
			//skip the key name
			info += keylen+1;
			//this is the old value for the key. skip over it
			while (*info && *info != '\\')
				info++;
		}
		else
		{
			//skip the key
			while (*info && *info != '\\')
				info++;

			//validate that its a value now
			if (*info++ != '\\')
				break;	//error
			//skip the value
			while (*info && *info != '\\')
				info++;

			//copy them over
			if (o + (info-l) >= e)
				break;	//exceeds maximum length
			while (l < info)
				*o++ = *l++;
		}
	}

	if (*info)
		Con_Warning("PF_infoadd: invalid source info\n");
	else if (!*value)
		; //nothing needed
	else if (!*key || strchr(key, '\\') || strchr(value, '\\'))
		Con_Warning("PF_infoadd: invalid key/value\n");
	else if (o + 2 + keylen + valuelen >= e)
		Con_Warning("PF_infoadd: length exceeds max\n");
	else
	{
		*o++ = '\\';
		memcpy(o, key, keylen);
		o += keylen;
		*o++ = '\\';
		memcpy(o, value, valuelen);
		o += keylen;
	}

	*o = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(destbuf);
}
static void PF_infoget(void)
{
	const char *info = G_STRING(OFS_PARM0);
	const char *key = G_STRING(OFS_PARM1);
	size_t keylen = strlen(key);
	while(*info)
	{
		if (*info++ != '\\')
			break;	//error / end-of-string

		if (!strncmp(info, key, keylen) && info[keylen] == '\\')
		{
			char *destbuf = PR_GetTempString(), *o = destbuf, *e = destbuf + STRINGTEMP_LENGTH - 1;

			//skip the key name
			info += keylen+1;
			//this is the old value for the key. copy it to the result
			while (*info && *info != '\\' && o < e)
				*o++ = *info++;
			*o++ = 0;

			//success!
			G_INT(OFS_RETURN) = PR_SetEngineString(destbuf);
			return;
		}
		else
		{
			//skip the key
			while (*info && *info != '\\')
				info++;

			//validate that its a value now
			if (*info++ != '\\')
				break;	//error
			//skip the value
			while (*info && *info != '\\')
				info++;
		}
	}
	G_INT(OFS_RETURN) = 0;

}
static void PF_strncmp(void)
{
	const char *a = G_STRING(OFS_PARM0);
	const char *b = G_STRING(OFS_PARM1);

	if (pr_argc > 2)
	{
		int len = G_FLOAT(OFS_PARM2);
		int aofs = pr_argc>3?G_FLOAT(OFS_PARM3):0;
		int bofs = pr_argc>4?G_FLOAT(OFS_PARM4):0;
		if (aofs < 0 || (aofs && aofs > (int)strlen(a)))
			aofs = strlen(a);
		if (bofs < 0 || (bofs && bofs > (int)strlen(b)))
			bofs = strlen(b);
		G_FLOAT(OFS_RETURN) = Q_strncmp(a + aofs, b, len);
	}
	else
		G_FLOAT(OFS_RETURN) = Q_strcmp(a, b);
}
static void PF_strncasecmp(void)
{
	const char *a = G_STRING(OFS_PARM0);
	const char *b = G_STRING(OFS_PARM1);

	if (pr_argc > 2)
	{
		int len = G_FLOAT(OFS_PARM2);
		int aofs = pr_argc>3?G_FLOAT(OFS_PARM3):0;
		int bofs = pr_argc>4?G_FLOAT(OFS_PARM4):0;
		if (aofs < 0 || (aofs && aofs > (int)strlen(a)))
			aofs = strlen(a);
		if (bofs < 0 || (bofs && bofs > (int)strlen(b)))
			bofs = strlen(b);
		G_FLOAT(OFS_RETURN) = q_strncasecmp(a + aofs, b, len);
	}
	else
		G_FLOAT(OFS_RETURN) = q_strcasecmp(a, b);
}
static void PF_strstrofs(void)
{
	const char *instr = G_STRING(OFS_PARM0);
	const char *match = G_STRING(OFS_PARM1);
	int firstofs = (pr_argc>2)?G_FLOAT(OFS_PARM2):0;

	if (firstofs && (firstofs < 0 || firstofs > (int)strlen(instr)))
	{
		G_FLOAT(OFS_RETURN) = -1;
		return;
	}

	match = strstr(instr+firstofs, match);
	if (!match)
		G_FLOAT(OFS_RETURN) = -1;
	else
		G_FLOAT(OFS_RETURN) = match - instr;
}
static void PF_strtrim(void)
{
	const char *str = G_STRING(OFS_PARM0);
	const char *end;
	char *news;
	size_t len;
	
	//figure out the new start
	while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')
		str++;

	//figure out the new end.
	end = str + strlen(str);
	while(end > str && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
		end--;

	//copy that substring into a tempstring.
	len = end - str;
	if (len >= STRINGTEMP_LENGTH)
		len = STRINGTEMP_LENGTH-1;

	news = PR_GetTempString();
	memcpy(news, str, len);
	news[len] = 0;

	G_INT(OFS_RETURN) = PR_SetEngineString(news);
}
static void PF_strreplace(void)
{
	char *resultbuf = PR_GetTempString();
	char *result = resultbuf;
	const char *search = G_STRING(OFS_PARM0);
	const char *replace = G_STRING(OFS_PARM1);
	const char *subject = G_STRING(OFS_PARM2);
	int searchlen = strlen(search);
	int replacelen = strlen(replace);

	if (searchlen)
	{
		while (*subject && result < resultbuf + sizeof(resultbuf) - replacelen - 2)
		{
			if (!strncmp(subject, search, searchlen))
			{
				subject += searchlen;
				memcpy(result, replace, replacelen);
				result += replacelen;
			}
			else
				*result++ = *subject++;
		}
		*result = 0;
		G_INT(OFS_RETURN) = PR_SetEngineString(resultbuf);
	}
	else
		G_INT(OFS_RETURN) = PR_SetEngineString(subject);
}
static void PF_strireplace(void)
{
	char *resultbuf = PR_GetTempString();
	char *result = resultbuf;
	const char *search = G_STRING(OFS_PARM0);
	const char *replace = G_STRING(OFS_PARM1);
	const char *subject = G_STRING(OFS_PARM2);
	int searchlen = strlen(search);
	int replacelen = strlen(replace);

	if (searchlen)
	{
		while (*subject && result < resultbuf + sizeof(resultbuf) - replacelen - 2)
		{
			//UTF-8-FIXME: case insensitivity is awkward...
			if (!q_strncasecmp(subject, search, searchlen))
			{
				subject += searchlen;
				memcpy(result, replace, replacelen);
				result += replacelen;
			}
			else
				*result++ = *subject++;
		}
		*result = 0;
		G_INT(OFS_RETURN) = PR_SetEngineString(resultbuf);
	}
	else
		G_INT(OFS_RETURN) = PR_SetEngineString(subject);
}


static void PF_sprintf_internal (const char *s, int firstarg, char *outbuf, int outbuflen)
{
	const char *s0;
	char *o = outbuf, *end = outbuf + outbuflen, *err;
	int width, precision, thisarg, flags;
	char formatbuf[16];
	char *f;
	int argpos = firstarg;
	int isfloat;
	static int dummyivec[3] = {0, 0, 0};
	static float dummyvec[3] = {0, 0, 0};

#define PRINTF_ALTERNATE 1
#define PRINTF_ZEROPAD 2
#define PRINTF_LEFT 4
#define PRINTF_SPACEPOSITIVE 8
#define PRINTF_SIGNPOSITIVE 16

	formatbuf[0] = '%';

#define GETARG_FLOAT(a) (((a)>=firstarg && (a)<pr_argc) ? (G_FLOAT(OFS_PARM0 + 3 * (a))) : 0)
#define GETARG_VECTOR(a) (((a)>=firstarg && (a)<pr_argc) ? (G_VECTOR(OFS_PARM0 + 3 * (a))) : dummyvec)
#define GETARG_INT(a) (((a)>=firstarg && (a)<pr_argc) ? (G_INT(OFS_PARM0 + 3 * (a))) : 0)
#define GETARG_INTVECTOR(a) (((a)>=firstarg && (a)<pr_argc) ? ((int*) G_VECTOR(OFS_PARM0 + 3 * (a))) : dummyivec)
#define GETARG_STRING(a) (((a)>=firstarg && (a)<pr_argc) ? (G_STRING(OFS_PARM0 + 3 * (a))) : "")

	for(;;)
	{
		s0 = s;
		switch(*s)
		{
			case 0:
				goto finished;
			case '%':
				++s;

				if(*s == '%')
					goto verbatim;

				// complete directive format:
				// %3$*1$.*2$ld
				
				width = -1;
				precision = -1;
				thisarg = -1;
				flags = 0;
				isfloat = -1;

				// is number following?
				if(*s >= '0' && *s <= '9')
				{
					width = strtol(s, &err, 10);
					if(!err)
					{
						Con_Warning("PF_sprintf: bad format string: %s\n", s0);
						goto finished;
					}
					if(*err == '$')
					{
						thisarg = width + (firstarg-1);
						width = -1;
						s = err + 1;
					}
					else
					{
						if(*s == '0')
						{
							flags |= PRINTF_ZEROPAD;
							if(width == 0)
								width = -1; // it was just a flag
						}
						s = err;
					}
				}

				if(width < 0)
				{
					for(;;)
					{
						switch(*s)
						{
							case '#': flags |= PRINTF_ALTERNATE; break;
							case '0': flags |= PRINTF_ZEROPAD; break;
							case '-': flags |= PRINTF_LEFT; break;
							case ' ': flags |= PRINTF_SPACEPOSITIVE; break;
							case '+': flags |= PRINTF_SIGNPOSITIVE; break;
							default:
								goto noflags;
						}
						++s;
					}
noflags:
					if(*s == '*')
					{
						++s;
						if(*s >= '0' && *s <= '9')
						{
							width = strtol(s, &err, 10);
							if(!err || *err != '$')
							{
								Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
								goto finished;
							}
							s = err + 1;
						}
						else
							width = argpos++;
						width = GETARG_FLOAT(width);
						if(width < 0)
						{
							flags |= PRINTF_LEFT;
							width = -width;
						}
					}
					else if(*s >= '0' && *s <= '9')
					{
						width = strtol(s, &err, 10);
						if(!err)
						{
							Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
							goto finished;
						}
						s = err;
						if(width < 0)
						{
							flags |= PRINTF_LEFT;
							width = -width;
						}
					}
					// otherwise width stays -1
				}

				if(*s == '.')
				{
					++s;
					if(*s == '*')
					{
						++s;
						if(*s >= '0' && *s <= '9')
						{
							precision = strtol(s, &err, 10);
							if(!err || *err != '$')
							{
								Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
								goto finished;
							}
							s = err + 1;
						}
						else
							precision = argpos++;
						precision = GETARG_FLOAT(precision);
					}
					else if(*s >= '0' && *s <= '9')
					{
						precision = strtol(s, &err, 10);
						if(!err)
						{
							Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
							goto finished;
						}
						s = err;
					}
					else
					{
						Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
						goto finished;
					}
				}

				for(;;)
				{
					switch(*s)
					{
						case 'h': isfloat = 1; break;
						case 'l': isfloat = 0; break;
						case 'L': isfloat = 0; break;
						case 'j': break;
						case 'z': break;
						case 't': break;
						default:
							goto nolength;
					}
					++s;
				}
nolength:

				// now s points to the final directive char and is no longer changed
				if (*s == 'p' || *s == 'P')
				{
					//%p is slightly different from %x.
					//always 8-bytes wide with 0 padding, always ints.
					flags |= PRINTF_ZEROPAD;
					if (width < 0) width = 8;
					if (isfloat < 0) isfloat = 0;
				}
				else if (*s == 'i')
				{
					//%i defaults to ints, not floats.
					if(isfloat < 0) isfloat = 0;
				}

				//assume floats, not ints.
				if(isfloat < 0)
					isfloat = 1;

				if(thisarg < 0)
					thisarg = argpos++;

				if(o < end - 1)
				{
					f = &formatbuf[1];
					if(*s != 's' && *s != 'c')
						if(flags & PRINTF_ALTERNATE) *f++ = '#';
					if(flags & PRINTF_ZEROPAD) *f++ = '0';
					if(flags & PRINTF_LEFT) *f++ = '-';
					if(flags & PRINTF_SPACEPOSITIVE) *f++ = ' ';
					if(flags & PRINTF_SIGNPOSITIVE) *f++ = '+';
					*f++ = '*';
					if(precision >= 0)
					{
						*f++ = '.';
						*f++ = '*';
					}
					if (*s == 'p')
						*f++ = 'x';
					else if (*s == 'P')
						*f++ = 'X';
					else
						*f++ = *s;
					*f++ = 0;

					if(width < 0) // not set
						width = 0;

					switch(*s)
					{
						case 'd': case 'i':
							if(precision < 0) // not set
								q_snprintf(o, end - o, formatbuf, width, (isfloat ? (int) GETARG_FLOAT(thisarg) : (int) GETARG_INT(thisarg)));
							else
								q_snprintf(o, end - o, formatbuf, width, precision, (isfloat ? (int) GETARG_FLOAT(thisarg) : (int) GETARG_INT(thisarg)));
							o += strlen(o);
							break;
						case 'o': case 'u': case 'x': case 'X': case 'p': case 'P':
							if(precision < 0) // not set
								q_snprintf(o, end - o, formatbuf, width, (isfloat ? (unsigned int) GETARG_FLOAT(thisarg) : (unsigned int) GETARG_INT(thisarg)));
							else
								q_snprintf(o, end - o, formatbuf, width, precision, (isfloat ? (unsigned int) GETARG_FLOAT(thisarg) : (unsigned int) GETARG_INT(thisarg)));
							o += strlen(o);
							break;
						case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
							if(precision < 0) // not set
								q_snprintf(o, end - o, formatbuf, width, (isfloat ? (double) GETARG_FLOAT(thisarg) : (double) GETARG_INT(thisarg)));
							else
								q_snprintf(o, end - o, formatbuf, width, precision, (isfloat ? (double) GETARG_FLOAT(thisarg) : (double) GETARG_INT(thisarg)));
							o += strlen(o);
							break;
						case 'v': case 'V':
							f[-2] += 'g' - 'v';
							if(precision < 0) // not set
								q_snprintf(o, end - o, va("%s %s %s", /* NESTED SPRINTF IS NESTED */ formatbuf, formatbuf, formatbuf),
									width, (isfloat ? (double) GETARG_VECTOR(thisarg)[0] : (double) GETARG_INTVECTOR(thisarg)[0]),
									width, (isfloat ? (double) GETARG_VECTOR(thisarg)[1] : (double) GETARG_INTVECTOR(thisarg)[1]),
									width, (isfloat ? (double) GETARG_VECTOR(thisarg)[2] : (double) GETARG_INTVECTOR(thisarg)[2])
								);
							else
								q_snprintf(o, end - o, va("%s %s %s", /* NESTED SPRINTF IS NESTED */ formatbuf, formatbuf, formatbuf),
									width, precision, (isfloat ? (double) GETARG_VECTOR(thisarg)[0] : (double) GETARG_INTVECTOR(thisarg)[0]),
									width, precision, (isfloat ? (double) GETARG_VECTOR(thisarg)[1] : (double) GETARG_INTVECTOR(thisarg)[1]),
									width, precision, (isfloat ? (double) GETARG_VECTOR(thisarg)[2] : (double) GETARG_INTVECTOR(thisarg)[2])
								);
							o += strlen(o);
							break;
						case 'c':
							//UTF-8-FIXME: figure it out yourself
//							if(flags & PRINTF_ALTERNATE)
							{
								if(precision < 0) // not set
									q_snprintf(o, end - o, formatbuf, width, (isfloat ? (unsigned int) GETARG_FLOAT(thisarg) : (unsigned int) GETARG_INT(thisarg)));
								else
									q_snprintf(o, end - o, formatbuf, width, precision, (isfloat ? (unsigned int) GETARG_FLOAT(thisarg) : (unsigned int) GETARG_INT(thisarg)));
								o += strlen(o);
							}
/*							else
							{
								unsigned int c = (isfloat ? (unsigned int) GETARG_FLOAT(thisarg) : (unsigned int) GETARG_INT(thisarg));
								char charbuf16[16];
								const char *buf = u8_encodech(c, NULL, charbuf16);
								if(!buf)
									buf = "";
								if(precision < 0) // not set
									precision = end - o - 1;
								o += u8_strpad(o, end - o, buf, (flags & PRINTF_LEFT) != 0, width, precision);
							}
*/							break;
						case 's':
							//UTF-8-FIXME: figure it out yourself
//							if(flags & PRINTF_ALTERNATE)
							{
								if(precision < 0) // not set
									q_snprintf(o, end - o, formatbuf, width, GETARG_STRING(thisarg));
								else
									q_snprintf(o, end - o, formatbuf, width, precision, GETARG_STRING(thisarg));
								o += strlen(o);
							}
/*							else
							{
								if(precision < 0) // not set
									precision = end - o - 1;
								o += u8_strpad(o, end - o, GETARG_STRING(thisarg), (flags & PRINTF_LEFT) != 0, width, precision);
							}
*/							break;
						default:
							Con_Warning("PF_sprintf: invalid format string: %s\n", s0);
							goto finished;
					}
				}
				++s;
				break;
			default:
verbatim:
				if(o < end - 1)
					*o++ = *s;
				s++;
				break;
		}
	}
finished:
	*o = 0;
}

static void PF_sprintf(void)
{
	char *outbuf = PR_GetTempString();
	PF_sprintf_internal(G_STRING(OFS_PARM0), 1, outbuf, STRINGTEMP_LENGTH);
	G_INT(OFS_RETURN) = PR_SetEngineString(outbuf);
}

//string tokenizing (gah)
#define MAXQCTOKENS 64
static struct {
	char *token;
	unsigned int start;
	unsigned int end;
} qctoken[MAXQCTOKENS];
unsigned int qctoken_count;

static void tokenize_flush(void)
{
	while(qctoken_count > 0)
	{
		qctoken_count--;
		free(qctoken[qctoken_count].token);
	}
	qctoken_count = 0;
}

static void PF_ArgC(void)
{
	G_FLOAT(OFS_RETURN) = qctoken_count;
}

static int tokenizeqc(const char *str, qboolean dpfuckage)
{
	//FIXME: if dpfuckage, then we should handle punctuation specially, as well as /*.
	const char *start = str;
	while(qctoken_count > 0)
	{
		qctoken_count--;
		free(qctoken[qctoken_count].token);
	}
	qctoken_count = 0;
	while (qctoken_count < MAXQCTOKENS)
	{
		/*skip whitespace here so the token's start is accurate*/
		while (*str && *(unsigned char*)str <= ' ')
			str++;

		if (!*str)
			break;

		qctoken[qctoken_count].start = str - start;
		str = COM_Parse(str);
		if (!str)
			break;

		qctoken[qctoken_count].token = strdup(com_token);

		qctoken[qctoken_count].end = str - start;
		qctoken_count++;
	}
	return qctoken_count;
}

/*KRIMZON_SV_PARSECLIENTCOMMAND added these two - note that for compatibility with DP, this tokenize builtin is veeery vauge and doesn't match the console*/
static void PF_Tokenize(void)
{
	G_FLOAT(OFS_RETURN) = tokenizeqc(G_STRING(OFS_PARM0), true);
}

static void PF_tokenize_console(void)
{
	G_FLOAT(OFS_RETURN) = tokenizeqc(G_STRING(OFS_PARM0), false);
}

static void PF_tokenizebyseparator(void)
{
	const char *str = G_STRING(OFS_PARM0);
	const char *sep[7];
	int seplen[7];
	int seps = 0, s;
	const char *start = str;
	int tlen;
	qboolean found = true;

	while (seps < pr_argc - 1 && seps < 7)
	{
		sep[seps] = G_STRING(OFS_PARM1 + seps*3);
		seplen[seps] = strlen(sep[seps]);
		seps++;
	}

	tokenize_flush();

	qctoken[qctoken_count].start = 0;
	if (*str)
	for(;;)
	{
		found = false;
		/*see if its a separator*/
		if (!*str)
		{
			qctoken[qctoken_count].end = str - start;
			found = true;
		}
		else
		{
			for (s = 0; s < seps; s++)
			{
				if (!strncmp(str, sep[s], seplen[s]))
				{
					qctoken[qctoken_count].end = str - start;
					str += seplen[s];
					found = true;
					break;
				}
			}
		}
		/*it was, split it out*/
		if (found)
		{
			tlen = qctoken[qctoken_count].end - qctoken[qctoken_count].start;
			qctoken[qctoken_count].token = malloc(tlen + 1);
			memcpy(qctoken[qctoken_count].token, start + qctoken[qctoken_count].start, tlen);
			qctoken[qctoken_count].token[tlen] = 0;

			qctoken_count++;

			if (*str && qctoken_count < MAXQCTOKENS)
				qctoken[qctoken_count].start = str - start;
			else
				break;
		}
		str++;
	}
	G_FLOAT(OFS_RETURN) = qctoken_count;
}

static void PF_argv_start_index(void)
{
	int idx = G_FLOAT(OFS_PARM0);

	/*negative indexes are relative to the end*/
	if (idx < 0)
		idx += qctoken_count;	

	if ((unsigned int)idx >= qctoken_count)
		G_FLOAT(OFS_RETURN) = -1;
	else
		G_FLOAT(OFS_RETURN) = qctoken[idx].start;
}

static void PF_argv_end_index(void)
{
	int idx = G_FLOAT(OFS_PARM0);

	/*negative indexes are relative to the end*/
	if (idx < 0)
		idx += qctoken_count;	

	if ((unsigned int)idx >= qctoken_count)
		G_FLOAT(OFS_RETURN) = -1;
	else
		G_FLOAT(OFS_RETURN) = qctoken[idx].end;
}

static void PF_ArgV(void)
{
	int idx = G_FLOAT(OFS_PARM0);

	/*negative indexes are relative to the end*/
	if (idx < 0)
		idx += qctoken_count;

	if ((unsigned int)idx >= qctoken_count)
		G_INT(OFS_RETURN) = 0;
	else
	{
		char *ret = PR_GetTempString();
		q_strlcpy(ret, qctoken[idx].token, STRINGTEMP_LENGTH);
		G_INT(OFS_RETURN) = PR_SetEngineString(ret);
	}
}

//conversions (mostly string)
static void PF_strtoupper(void)
{
	const char *in = G_STRING(OFS_PARM0);
	char *out, *result = PR_GetTempString();
	for (out = result; *in && out < result+STRINGTEMP_LENGTH-1;)
		*out++ = q_toupper(*in++);
	*out = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_strtolower(void)
{
	const char *in = G_STRING(OFS_PARM0);
	char *out, *result = PR_GetTempString();
	for (out = result; *in && out < result+STRINGTEMP_LENGTH-1;)
		*out++ = q_tolower(*in++);
	*out = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
#include <time.h>
static void PF_strftime(void)
{
	const char *in = G_STRING(OFS_PARM1);
	char *result = PR_GetTempString();

	time_t ctime;
	struct tm *tm;

	ctime = time(NULL);

	if (G_FLOAT(OFS_PARM0))
		tm = localtime(&ctime);
	else
		tm = gmtime(&ctime);

#ifdef _WIN32
	//msvc sucks. this is a crappy workaround.
	if (!strcmp(in, "%R"))
		in = "%H:%M";
	else if (!strcmp(in, "%F"))
		in = "%Y-%m-%d";
#endif

	strftime(result, STRINGTEMP_LENGTH, in, tm);

	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_stof(void)
{
	G_FLOAT(OFS_RETURN) = atoi(G_STRING(OFS_PARM0));
}
static void PF_stov(void)
{
	const char *s = G_STRING(OFS_PARM0);
	s = COM_Parse(s);
	G_VECTOR(OFS_RETURN)[0] = atof(com_token);
	s = COM_Parse(s);
	G_VECTOR(OFS_RETURN)[1] = atof(com_token);
	s = COM_Parse(s);
	G_VECTOR(OFS_RETURN)[2] = atof(com_token);
}
static void PF_stoi(void)
{
	G_INT(OFS_RETURN) = atoi(G_STRING(OFS_PARM0));
}
static void PF_itos(void)
{
	char *result = PR_GetTempString();
	q_snprintf(result, STRINGTEMP_LENGTH, "%i", G_INT(OFS_PARM0));
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_etos(void)
{	//yes, this is lame
	char *result = PR_GetTempString();
	q_snprintf(result, STRINGTEMP_LENGTH, "entity %i", G_EDICTNUM(OFS_PARM0));
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_stoh(void)
{
	G_INT(OFS_RETURN) = strtoul(G_STRING(OFS_PARM0), NULL, 16);
}
static void PF_htos(void)
{
	char *result = PR_GetTempString();
	q_snprintf(result, STRINGTEMP_LENGTH, "%x", G_INT(OFS_PARM0));
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_ftoi(void)
{
	G_INT(OFS_RETURN) = G_FLOAT(OFS_PARM0);
}
static void PF_itof(void)
{
	G_FLOAT(OFS_RETURN) = G_INT(OFS_PARM0);
}

//collision stuff
static void PF_tracebox(void)
{	//alternative version of traceline that just passes on two extra args. trivial really.
	float	*v1, *mins, *maxs, *v2;
	trace_t	trace;
	int	nomonsters;
	edict_t	*ent;

	v1 = G_VECTOR(OFS_PARM0);
	mins = G_VECTOR(OFS_PARM1);
	maxs = G_VECTOR(OFS_PARM2);
	v2 = G_VECTOR(OFS_PARM3);
	nomonsters = G_FLOAT(OFS_PARM4);
	ent = G_EDICT(OFS_PARM5);

	/* FIXME FIXME FIXME: Why do we hit this with certain progs.dat ?? */
	if (developer.value) {
	  if (IS_NAN(v1[0]) || IS_NAN(v1[1]) || IS_NAN(v1[2]) ||
	      IS_NAN(v2[0]) || IS_NAN(v2[1]) || IS_NAN(v2[2])) {
	    Con_Warning ("NAN in traceline:\nv1(%f %f %f) v2(%f %f %f)\nentity %d\n",
		      v1[0], v1[1], v1[2], v2[0], v2[1], v2[2], NUM_FOR_EDICT(ent));
	  }
	}

	if (IS_NAN(v1[0]) || IS_NAN(v1[1]) || IS_NAN(v1[2]))
		v1[0] = v1[1] = v1[2] = 0;
	if (IS_NAN(v2[0]) || IS_NAN(v2[1]) || IS_NAN(v2[2]))
		v2[0] = v2[1] = v2[2] = 0;

	trace = SV_Move (v1, mins, maxs, v2, nomonsters, ent);

	pr_global_struct->trace_allsolid = trace.allsolid;
	pr_global_struct->trace_startsolid = trace.startsolid;
	pr_global_struct->trace_fraction = trace.fraction;
	pr_global_struct->trace_inwater = trace.inwater;
	pr_global_struct->trace_inopen = trace.inopen;
	VectorCopy (trace.endpos, pr_global_struct->trace_endpos);
	VectorCopy (trace.plane.normal, pr_global_struct->trace_plane_normal);
	pr_global_struct->trace_plane_dist =  trace.plane.dist;
	if (trace.ent)
		pr_global_struct->trace_ent = EDICT_TO_PROG(trace.ent);
	else
		pr_global_struct->trace_ent = EDICT_TO_PROG(sv.edicts);
}
static void PF_TraceToss(void)
{
	extern cvar_t sv_maxvelocity, sv_gravity;
	int i;
	float gravity;
	vec3_t move, end;
	trace_t trace;
	eval_t	*val;

	vec3_t origin, velocity;

	edict_t *tossent, *ignore;
	tossent = G_EDICT(OFS_PARM0);
	if (tossent == sv.edicts)
		Con_Warning("tracetoss: can not use world entity\n");
	ignore = G_EDICT(OFS_PARM1);

	val = GetEdictFieldValue(tossent, pr_extfields.gravity);
	if (val && val->_float)
		gravity = val->_float;
	else
		gravity = 1;
	gravity *= sv_gravity.value * 0.05;

	VectorCopy (tossent->v.origin, origin);
	VectorCopy (tossent->v.velocity, velocity);

	SV_CheckVelocity (tossent);

	for (i = 0;i < 200;i++) // LordHavoc: sanity check; never trace more than 10 seconds
	{
		velocity[2] -= gravity;
		VectorScale (velocity, 0.05, move);
		VectorAdd (origin, move, end);
		trace = SV_Move (origin, tossent->v.mins, tossent->v.maxs, end, MOVE_NORMAL, tossent);
		VectorCopy (trace.endpos, origin);

		if (trace.fraction < 1 && trace.ent && trace.ent != ignore)
			break;

		if (VectorLength(velocity) > sv_maxvelocity.value)
		{
//			Con_DPrintf("Slowing %s\n", PR_GetString(w->progs, tossent->v->classname));
			VectorScale (velocity, sv_maxvelocity.value/VectorLength(velocity), velocity);
		}
	}

	trace.fraction = 0; // not relevant
	
	//and return those as globals.
	pr_global_struct->trace_allsolid = trace.allsolid;
	pr_global_struct->trace_startsolid = trace.startsolid;
	pr_global_struct->trace_fraction = trace.fraction;
	pr_global_struct->trace_inwater = trace.inwater;
	pr_global_struct->trace_inopen = trace.inopen;
	VectorCopy (trace.endpos, pr_global_struct->trace_endpos);
	VectorCopy (trace.plane.normal, pr_global_struct->trace_plane_normal);
	pr_global_struct->trace_plane_dist =  trace.plane.dist;
	if (trace.ent)
		pr_global_struct->trace_ent = EDICT_TO_PROG(trace.ent);
	else
		pr_global_struct->trace_ent = EDICT_TO_PROG(sv.edicts);
}

//model stuff
static void PF_frameforname(void)
{
	unsigned int modelindex	= G_FLOAT(OFS_PARM0);
	const char *framename	= G_STRING(OFS_PARM1);
	qmodel_t *mod = (modelindex < MAX_MODELS)?sv.models[modelindex]:NULL;
	aliashdr_t *alias;

	G_FLOAT(OFS_RETURN) = -1;
	if (mod && mod->type == mod_alias && (alias = Mod_Extradata(mod)))
	{
		int i;
		for (i = 0; i < alias->numframes; i++)
		{
			if (!strcmp(alias->frames[i].name, framename))
			{
				G_FLOAT(OFS_RETURN) = i;
				break;
			}
		}
	}
}
static void PF_frametoname(void)
{
	unsigned int modelindex	= G_FLOAT(OFS_PARM0);
	unsigned int framenum	= G_FLOAT(OFS_PARM1);
	qmodel_t *mod = (modelindex < MAX_MODELS)?sv.models[modelindex]:NULL;
	aliashdr_t *alias;

	if (mod && mod->type == mod_alias && (alias = Mod_Extradata(mod)) && framenum < (unsigned int)alias->numframes)
		G_INT(OFS_RETURN) = PR_SetEngineString(alias->frames[framenum].name);
	else
		G_INT(OFS_RETURN) = 0;
}
static void PF_frameduration(void)
{
	unsigned int modelindex	= G_FLOAT(OFS_PARM0);
	unsigned int framenum	= G_FLOAT(OFS_PARM1);
	qmodel_t *mod = (modelindex < MAX_MODELS)?sv.models[modelindex]:NULL;
	aliashdr_t *alias;

	if (mod && mod->type == mod_alias && (alias = Mod_Extradata(mod)) && framenum < (unsigned int)alias->numframes)
		G_FLOAT(OFS_RETURN) = alias->frames[framenum].numposes * alias->frames[framenum].interval;
}
static void PF_getsurfacenumpoints(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	unsigned int modelindex = ed->v.modelindex;
	qmodel_t *mod = (modelindex < MAX_MODELS)?sv.models[modelindex]:NULL;

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces)
	{
		surfidx += mod->firstmodelsurface;
		G_FLOAT(OFS_RETURN) = mod->surfaces[surfidx].numedges;
	}
	else
		G_FLOAT(OFS_RETURN) = 0;
}
static mvertex_t *PF_getsurfacevertex(qmodel_t *mod, msurface_t *surf, unsigned int vert)
{
	signed int edge = mod->surfedges[vert+surf->firstedge];
	if (edge >= 0)
		return &mod->vertexes[mod->edges[edge].v[0]];
	else
		return &mod->vertexes[mod->edges[-edge].v[1]];
}
static void PF_getsurfacepoint(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	unsigned int point		= G_FLOAT(OFS_PARM2);
	unsigned int modelindex = ed->v.modelindex;
	qmodel_t *mod = (modelindex < MAX_MODELS)?sv.models[modelindex]:NULL;

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces && point < (unsigned int)mod->surfaces[surfidx].numedges)
	{
		mvertex_t *v = PF_getsurfacevertex(mod, &mod->surfaces[surfidx+mod->firstmodelsurface], point);
		VectorCopy(v->position, G_VECTOR(OFS_RETURN));
	}
	else
	{
		G_FLOAT(OFS_RETURN+0) = 0;
		G_FLOAT(OFS_RETURN+1) = 0;
		G_FLOAT(OFS_RETURN+2) = 0;
	}
}
static void PF_getsurfacenumtriangles(void)
{	//for q3bsp compat (which this engine doesn't support, so its fairly simple)
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	unsigned int modelindex = ed->v.modelindex;
	qmodel_t *mod = (modelindex < MAX_MODELS)?sv.models[modelindex]:NULL;

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces)
		G_FLOAT(OFS_RETURN) = (mod->surfaces[surfidx+mod->firstmodelsurface].numedges-2);	//q1bsp is only triangle fans
	else
		G_FLOAT(OFS_RETURN) = 0;
}
static void PF_getsurfacetriangle(void)
{	//for q3bsp compat (which this engine doesn't support, so its fairly simple)
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	unsigned int triangleidx= G_FLOAT(OFS_PARM2);
	unsigned int modelindex = ed->v.modelindex;
	qmodel_t *mod = (modelindex < MAX_MODELS)?sv.models[modelindex]:NULL;

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces && triangleidx < (unsigned int)mod->surfaces[surfidx].numedges-2)
	{
		G_FLOAT(OFS_RETURN+0) = 0;
		G_FLOAT(OFS_RETURN+1) = triangleidx+1;
		G_FLOAT(OFS_RETURN+2) = triangleidx+2;
	}
	else
	{
		G_FLOAT(OFS_RETURN+0) = 0;
		G_FLOAT(OFS_RETURN+1) = 0;
		G_FLOAT(OFS_RETURN+2) = 0;
	}
}
static void PF_getsurfacenormal(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	unsigned int modelindex = ed->v.modelindex;
	qmodel_t *mod = (modelindex < MAX_MODELS)?sv.models[modelindex]:NULL;

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces)
	{
		surfidx += mod->firstmodelsurface;
		VectorCopy(mod->surfaces[surfidx].plane->normal, G_VECTOR(OFS_RETURN));
		if (mod->surfaces[surfidx].flags & SURF_PLANEBACK)
			VectorInverse(G_VECTOR(OFS_RETURN));
	}
	else
		G_FLOAT(OFS_RETURN) = 0;
}
static void PF_getsurfacetexture(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	unsigned int modelindex = ed->v.modelindex;
	qmodel_t *mod = (modelindex < MAX_MODELS)?sv.models[modelindex]:NULL;

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces)
	{
		surfidx += mod->firstmodelsurface;
		G_INT(OFS_RETURN) = PR_SetEngineString(mod->surfaces[surfidx].texinfo->texture->name);
	}
	else
		G_INT(OFS_RETURN) = 0;
}

#define TriangleNormal(a,b,c,n) ( \
	(n)[0] = ((a)[1] - (b)[1]) * ((c)[2] - (b)[2]) - ((a)[2] - (b)[2]) * ((c)[1] - (b)[1]), \
	(n)[1] = ((a)[2] - (b)[2]) * ((c)[0] - (b)[0]) - ((a)[0] - (b)[0]) * ((c)[2] - (b)[2]), \
	(n)[2] = ((a)[0] - (b)[0]) * ((c)[1] - (b)[1]) - ((a)[1] - (b)[1]) * ((c)[0] - (b)[0]) \
	)
static float getsurface_clippointpoly(qmodel_t *model, msurface_t *surf, vec3_t point, vec3_t bestcpoint, float bestdist)
{
	int e, edge;
	vec3_t edgedir, edgenormal, cpoint, temp;
	mvertex_t *v1, *v2;
	float dist = DotProduct(point, surf->plane->normal) - surf->plane->dist;
	//don't care about SURF_PLANEBACK, the maths works out the same.

	if (dist*dist < bestdist)
	{	//within a specific range
		//make sure it's within the poly
		VectorMA(point, dist, surf->plane->normal, cpoint);
		for (e = surf->firstedge+surf->numedges; e > surf->firstedge; edge++)
		{
			edge = model->surfedges[--e];
			if (edge < 0)
			{
				v1 = &model->vertexes[model->edges[-edge].v[0]];
				v2 = &model->vertexes[model->edges[-edge].v[1]];
			}
			else
			{
				v2 = &model->vertexes[model->edges[edge].v[0]];
				v1 = &model->vertexes[model->edges[edge].v[1]];
			}
			
			VectorSubtract(v1->position, v2->position, edgedir);
			CrossProduct(edgedir, surf->plane->normal, edgenormal);
			if (!(surf->flags & SURF_PLANEBACK))
			{
				VectorSubtract(vec3_origin, edgenormal, edgenormal);
			}
			VectorNormalize(edgenormal);

			dist = DotProduct(v1->position, edgenormal) - DotProduct(cpoint, edgenormal);
			if (dist < 0)
				VectorMA(cpoint, dist, edgenormal, cpoint);
		}

		VectorSubtract(cpoint, point, temp);
		dist = DotProduct(temp, temp);
		if (dist < bestdist)
		{
			bestdist = dist;
			VectorCopy(cpoint, bestcpoint);
		}
	}
	return bestdist;
}

// #438 float(entity e, vector p) getsurfacenearpoint (DP_QC_GETSURFACE)
static void PF_getsurfacenearpoint(void)
{
	qmodel_t *model;
	edict_t *ent;
	msurface_t *surf;
	int i;
	float *point;
	unsigned int u;

	vec3_t cpoint = {0,0,0};
	float bestdist = 0x7fffffff, dist;
	int bestsurf = -1;

	ent = G_EDICT(OFS_PARM0);
	point = G_VECTOR(OFS_PARM1);

	G_FLOAT(OFS_RETURN) = -1;

	u = ent->v.modelindex;
	model = (u>=MAX_MODELS)?NULL:sv.models[u];

	if (!model || model->type != mod_brush || model->needload)
		return;

	bestdist = 256;

	//all polies, we can skip parts. special case.
	surf = model->surfaces + model->firstmodelsurface;
	for (i = 0; i < model->nummodelsurfaces; i++, surf++)
	{
		dist = getsurface_clippointpoly(model, surf, point, cpoint, bestdist);
		if (dist < bestdist)
		{
			bestdist = dist;
			bestsurf = i;
		}
	}
	G_FLOAT(OFS_RETURN) = bestsurf;
}

// #439 vector(entity e, float s, vector p) getsurfaceclippedpoint (DP_QC_GETSURFACE)
static void PF_getsurfaceclippedpoint(void)
{
	qmodel_t *model;
	edict_t *ent;
	msurface_t *surf;
	float *point;
	int surfnum;
	unsigned int u;

	float *result = G_VECTOR(OFS_RETURN);

	ent = G_EDICT(OFS_PARM0);
	surfnum = G_FLOAT(OFS_PARM1);
	point = G_VECTOR(OFS_PARM2);

	VectorCopy(point, result);

	u = ent->v.modelindex;
	model = (u>=MAX_MODELS)?NULL:sv.models[u];

	if (!model || model->type != mod_brush || model->needload)
		return;
	if (surfnum >= model->nummodelsurfaces)
		return;

	//all polies, we can skip parts. special case.
	surf = model->surfaces + model->firstmodelsurface + surfnum;
	getsurface_clippointpoly(model, surf, point, result, 0x7fffffff);
}

static void PF_getsurfacepointattribute(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int surfidx	= G_FLOAT(OFS_PARM1);
	unsigned int point		= G_FLOAT(OFS_PARM2);
	unsigned int attribute	= G_FLOAT(OFS_PARM3);

	unsigned int modelindex = ed->v.modelindex;
	qmodel_t *mod = (modelindex < MAX_MODELS)?sv.models[modelindex]:NULL;

	if (mod && mod->type == mod_brush && !mod->needload && surfidx < (unsigned int)mod->nummodelsurfaces && point < (unsigned int)mod->surfaces[mod->firstmodelsurface+surfidx].numedges)
	{
		msurface_t *fa = &mod->surfaces[surfidx+mod->firstmodelsurface];
		mvertex_t *v = PF_getsurfacevertex(mod, fa, point);
		switch(attribute)
		{
		default:
			Con_Warning("PF_getsurfacepointattribute: attribute %u not supported\n", attribute);
			G_FLOAT(OFS_RETURN+0) = 0;
			G_FLOAT(OFS_RETURN+1) = 0;
			G_FLOAT(OFS_RETURN+2) = 0;
			break;
		case 0:	//xyz coord
			VectorCopy(v->position, G_VECTOR(OFS_RETURN));
			break;
		case 1:	//s dir
		case 2:	//t dir
			{
				//figure out how similar to the normal it is, and negate any influence, so that its perpendicular
				float sc = -DotProduct(fa->plane->normal, fa->texinfo->vecs[attribute-1]);
				VectorMA(fa->texinfo->vecs[attribute-1], sc, fa->plane->normal, G_VECTOR(OFS_RETURN));
				VectorNormalize(G_VECTOR(OFS_RETURN));
			}
			break;
		case 3: //normal
			VectorCopy(fa->plane->normal, G_VECTOR(OFS_RETURN));
			if (fa->flags & SURF_PLANEBACK)
				VectorInverse(G_VECTOR(OFS_RETURN));
			break;
		case 4: //st coord
			G_FLOAT(OFS_RETURN+0) = (DotProduct(v->position, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3]) / fa->texinfo->texture->width;
			G_FLOAT(OFS_RETURN+1) = (DotProduct(v->position, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3]) / fa->texinfo->texture->height;
			G_FLOAT(OFS_RETURN+2) = 0;
			break;
		case 5: //lmst coord, not actually very useful
#define BLOCK_WIDTH 128
#define BLOCK_HEIGHT 128
			G_FLOAT(OFS_RETURN+0) = (DotProduct(v->position, fa->texinfo->vecs[0]) + fa->texinfo->vecs[0][3] - fa->texturemins[0] + fa->light_s*16+8) / (BLOCK_WIDTH*16);
			G_FLOAT(OFS_RETURN+1) = (DotProduct(v->position, fa->texinfo->vecs[1]) + fa->texinfo->vecs[1][3] - fa->texturemins[1] + fa->light_t*16+8) / (BLOCK_HEIGHT*16);
			G_FLOAT(OFS_RETURN+2) = 0;
			break;
		case 6: //colour
			G_FLOAT(OFS_RETURN+0) = 1;
			G_FLOAT(OFS_RETURN+1) = 1;
			G_FLOAT(OFS_RETURN+2) = 1;
			break;
		}
	}
	else
	{
		G_FLOAT(OFS_RETURN+0) = 0;
		G_FLOAT(OFS_RETURN+1) = 0;
		G_FLOAT(OFS_RETURN+2) = 0;
	}
}
static void PF_sv_getlight(void)
{
	float *point = G_VECTOR(OFS_PARM0);
	//FIXME: seems like quakespasm doesn't do lits for model lighting, so we won't either.
	G_FLOAT(OFS_RETURN+0) = G_FLOAT(OFS_RETURN+1) = G_FLOAT(OFS_RETURN+2) = R_LightPoint(point) / 255.0;
}

//server/client stuff
static void PF_checkcommand(void)
{
	const char *name = G_STRING(OFS_PARM0);
	if (Cmd_Exists(name))
		G_FLOAT(OFS_RETURN) = 1;
	else if (Cmd_AliasExists(name))
		G_FLOAT(OFS_RETURN) = 2;
	else if (Cvar_FindVar(name))
		G_FLOAT(OFS_RETURN) = 3;
	else
		G_FLOAT(OFS_RETURN) = 0;
}
static void PF_setcolors(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	int newcol				= G_FLOAT(OFS_PARM1);
	unsigned int i			= NUM_FOR_EDICT(ed)-1;
	if (i >= (unsigned int)svs.maxclients)
	{
		Con_Printf ("tried to setcolor a non-client\n");
		return;
	}
	//FIXME: should we allow this for inactive players?

//update it
	svs.clients[i].colors = newcol;
	svs.clients[i].edict->v.team = (newcol&15) + 1;

// send notification to all clients
	MSG_WriteByte (&sv.reliable_datagram, svc_updatecolors);
	MSG_WriteByte (&sv.reliable_datagram, i);
	MSG_WriteByte (&sv.reliable_datagram, newcol);
}
static void PF_clientcommand(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	const char *str			= G_STRING(OFS_PARM1);
	unsigned int i			= NUM_FOR_EDICT(ed)-1;
	if (i < (unsigned int)svs.maxclients && svs.clients[i].active)
	{
		client_t *ohc = host_client;
		host_client = &svs.clients[i];
		Cmd_ExecuteString (str, src_client);
		host_client = ohc;
	}
	else
		Con_Printf("PF_clientcommand: not a client\n");
}
static void PF_clienttype(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int i			= NUM_FOR_EDICT(ed)-1;
	if (i >= (unsigned int)svs.maxclients)
	{
		G_FLOAT(OFS_RETURN) = 3;	//CLIENTTYPE_NOTACLIENT
		return;
	}
	if (svs.clients[i].active)
	{
		if (svs.clients[i].netconnection)
			G_FLOAT(OFS_RETURN) = 1;//CLIENTTYPE_REAL;
		else
			G_FLOAT(OFS_RETURN) = 2;//CLIENTTYPE_BOT;
	}
	else
		G_FLOAT(OFS_RETURN) = 0;//CLIENTTYPE_DISCONNECTED;
}
static void PF_spawnclient(void)
{
	edict_t *ent;
	unsigned int i;
	if (svs.maxclients)
	for (i = svs.maxclients; i --> 0; )
	{
		if (!svs.clients[i].active)
		{
			svs.clients[i].netconnection = NULL;	//botclients have no net connection, obviously.
			SV_ConnectClient(i);
			svs.clients[i].spawned = true;
			ent = svs.clients[i].edict;
			memset (&ent->v, 0, progs->entityfields * 4);
			ent->v.colormap = NUM_FOR_EDICT(ent);
			ent->v.team = (svs.clients[i].colors & 15) + 1;
			ent->v.netname = PR_SetEngineString(svs.clients[i].name);
			RETURN_EDICT(ent);
			return;
		}
	}
	RETURN_EDICT(sv.edicts);
}
static void PF_dropclient(void)
{
	edict_t	*ed				= G_EDICT(OFS_PARM0);
	unsigned int i			= NUM_FOR_EDICT(ed)-1;
	if (i < (unsigned int)svs.maxclients && svs.clients[i].active)
	{	//FIXME: should really set a flag or something, to avoid recursion issues.
		client_t *ohc = host_client;
		host_client = &svs.clients[i];
		SV_DropClient (false);
		host_client = ohc;
	}
}

//console/cvar stuff
static void PF_print(void)
{
	int i; 
	for (i = 0; i < pr_argc; i++)
		Con_Printf("%s", G_STRING(OFS_PARM0 + i*3));
}
static void PF_cvar_string(void)
{
	const char *name = G_STRING(OFS_PARM0);
	cvar_t *var = Cvar_FindVar(name);
	if (var && var->string)
	{
		//cvars can easily change values.
		//this would result in leaks/exploits/slowdowns if the qc spams calls to cvar_string+changes.
		//so keep performance consistent, even if this is going to be slower.
		char *temp = PR_GetTempString();
		q_strlcpy(temp, var->string, STRINGTEMP_LENGTH);
		G_INT(OFS_RETURN) = PR_SetEngineString(temp);
	}
	else
		G_INT(OFS_RETURN) = 0;
}
static void PF_cvar_defstring(void)
{
	const char *name = G_STRING(OFS_PARM0);
	cvar_t *var = Cvar_FindVar(name);
	if (var && var->default_string)
		G_INT(OFS_RETURN) = PR_SetEngineString(var->default_string);
	else
		G_INT(OFS_RETURN) = 0;
}
static void PF_cvar_type(void)
{
	const char	*str = G_STRING(OFS_PARM0);
	int ret = 0;
	cvar_t *v;

	v = Cvar_FindVar(str);
	if (v)
	{
		ret |= 1; // CVAR_EXISTS
		if(v->flags & CVAR_ARCHIVE)
			ret |= 2; // CVAR_TYPE_SAVED
//		if(v->flags & CVAR_NOTFROMSERVER)
//			ret |= 4; // CVAR_TYPE_PRIVATE
		if(!(v->flags & CVAR_USERDEFINED))
			ret |= 8; // CVAR_TYPE_ENGINE
//		if (v->description)
//			ret |= 16; // CVAR_TYPE_HASDESCRIPTION
	}
	G_FLOAT(OFS_RETURN) = ret;
}
static void PF_cvar_description(void)
{	//quakespasm does not support cvar descriptions. we provide this stub to avoid crashes.
	G_INT(OFS_RETURN) = 0;
}
static void PF_registercvar(void)
{
	const char *name = G_STRING(OFS_PARM0);
	const char *value = (pr_argc>1)?G_STRING(OFS_PARM0):"";
	Cvar_Create(name, value);
}

//temp entities + networking
static void PF_WriteString2(void)
{	//writes a string without the null. a poor-man's strcat.
	const char *string = G_STRING(OFS_PARM0);
	SZ_Write (WriteDest(), string, Q_strlen(string));
}
static void PF_WriteFloat(void)
{	//curiously, this was missing in vanilla.
	MSG_WriteFloat(WriteDest(), G_FLOAT(OFS_PARM0));
}
static void PF_sv_te_blooddp(void)
{	//blood is common enough that we should emulate it for when engines do actually support it.
	float *org = G_VECTOR(OFS_PARM0);
	float *dir = G_VECTOR(OFS_PARM1);
	float color = 73;
	float count = G_FLOAT(OFS_PARM2);
	SV_StartParticle (org, dir, color, count);
}
static void PF_sv_te_bloodqw(void)
{	//qw tried to strip a lot.
	float *org = G_VECTOR(OFS_PARM0);
	float *dir = vec3_origin;
	float color = 73;
	float count = G_FLOAT(OFS_PARM1)*20;
	SV_StartParticle (org, dir, color, count);
}
static void PF_sv_te_lightningblood(void)
{	//a qw builtin, to replace particle.
	float *org = G_VECTOR(OFS_PARM0);
	vec3_t dir = {0, 0, -100};
	float color = 20;
	float count = 225;
	SV_StartParticle (org, dir, color, count);
}
static void PF_sv_te_spike(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_SPIKE);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PVS_U, org, 0, 0);
}
static void PF_sv_te_superspike(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_SUPERSPIKE);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PVS_U, org, 0, 0);
}
static void PF_sv_te_gunshot(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	//float count = G_FLOAT(OFS_PARM1)*20;
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_GUNSHOT);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PVS_U, org, 0, 0);
}
static void PF_sv_te_explosion(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_EXPLOSION);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_tarexplosion(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_TAREXPLOSION);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_lightning1(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_LIGHTNING1);
	MSG_WriteShort(&sv.datagram, NUM_FOR_EDICT(ed));
	MSG_WriteCoord(&sv.datagram, start[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[2], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
static void PF_sv_te_lightning2(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_LIGHTNING2);
	MSG_WriteShort(&sv.datagram, NUM_FOR_EDICT(ed));
	MSG_WriteCoord(&sv.datagram, start[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[2], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
static void PF_sv_te_wizspike(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_WIZSPIKE);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_knightspike(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_KNIGHTSPIKE);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_lightning3(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_LIGHTNING3);
	MSG_WriteShort(&sv.datagram, NUM_FOR_EDICT(ed));
	MSG_WriteCoord(&sv.datagram, start[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, start[2], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, end[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
static void PF_sv_te_lavasplash(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.datagram, svc_temp_entity);
	MSG_WriteByte(&sv.datagram, TE_LAVASPLASH);
	MSG_WriteCoord(&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.datagram, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_teleport(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	MSG_WriteByte(&sv.multicast, svc_temp_entity);
	MSG_WriteByte(&sv.multicast, TE_TELEPORT);
	MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_explosion2(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	int palstart = G_FLOAT(OFS_PARM1);
	int palcount = G_FLOAT(OFS_PARM1);
	MSG_WriteByte(&sv.multicast, svc_temp_entity);
	MSG_WriteByte(&sv.multicast, TE_EXPLOSION2);
	MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
	MSG_WriteByte(&sv.multicast, palstart);
	MSG_WriteByte(&sv.multicast, palcount);
	SV_Multicast(MULTICAST_PHS_U, org, 0, 0);
}
static void PF_sv_te_beam(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	float *start = G_VECTOR(OFS_PARM1);
	float *end = G_VECTOR(OFS_PARM2);
	MSG_WriteByte(&sv.multicast, svc_temp_entity);
	MSG_WriteByte(&sv.multicast, TE_BEAM);
	MSG_WriteShort(&sv.multicast, NUM_FOR_EDICT(ed));
	MSG_WriteCoord(&sv.multicast, start[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, start[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, start[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[2], sv.protocolflags);
	SV_Multicast(MULTICAST_PHS_U, start, 0, 0);
}
#ifdef PSET_SCRIPT
static void PF_sv_te_particlerain(void)
{
	float *min = G_VECTOR(OFS_PARM0);
	float *max = G_VECTOR(OFS_PARM1);
	float *velocity = G_VECTOR(OFS_PARM2);
	float count = G_FLOAT(OFS_PARM3);
	float colour = G_FLOAT(OFS_PARM4);

	if (count < 1)
		return;
	if (count > 65535)
		count = 65535;

	MSG_WriteByte(&sv.multicast, svc_temp_entity);
	MSG_WriteByte(&sv.multicast, TEDP_PARTICLERAIN);
	MSG_WriteCoord(&sv.multicast, min[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, min[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, min[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[2], sv.protocolflags);
	MSG_WriteShort(&sv.multicast, count);
	MSG_WriteByte(&sv.multicast, colour);

	SV_Multicast (MULTICAST_ALL_U, NULL, 0, PEXT2_REPLACEMENTDELTAS);
}
static void PF_sv_te_particlesnow(void)
{
	float *min = G_VECTOR(OFS_PARM0);
	float *max = G_VECTOR(OFS_PARM1);
	float *velocity = G_VECTOR(OFS_PARM2);
	float count = G_FLOAT(OFS_PARM3);
	float colour = G_FLOAT(OFS_PARM4);

	if (count < 1)
		return;
	if (count > 65535)
		count = 65535;

	MSG_WriteByte(&sv.multicast, svc_temp_entity);
	MSG_WriteByte(&sv.multicast, TEDP_PARTICLESNOW);
	MSG_WriteCoord(&sv.multicast, min[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, min[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, min[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, max[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, velocity[2], sv.protocolflags);
	MSG_WriteShort(&sv.multicast, count);
	MSG_WriteByte(&sv.multicast, colour);

	SV_Multicast (MULTICAST_ALL_U, NULL, 0, PEXT2_REPLACEMENTDELTAS);
}
#else
#define PF_sv_te_particlerain PF_void_stub
#define PF_sv_te_particlesnow PF_void_stub
#endif
#define PF_sv_te_bloodshower PF_void_stub
#define PF_sv_te_explosionrgb PF_void_stub
#define PF_sv_te_particlecube PF_void_stub
#define PF_sv_te_spark PF_void_stub
#define PF_sv_te_gunshotquad PF_sv_te_gunshot
#define PF_sv_te_spikequad PF_sv_te_spike
#define PF_sv_te_superspikequad PF_sv_te_superspike
#define PF_sv_te_explosionquad PF_sv_te_explosion
#define PF_sv_te_smallflash PF_void_stub
#define PF_sv_te_customflash PF_void_stub
#define PF_sv_te_plasmaburn PF_sv_te_tarexplosion
#define PF_sv_effect PF_void_stub

static void PF_sv_pointsound(void)
{
	float *origin = G_VECTOR(OFS_PARM0);
	const char *sample = G_STRING(OFS_PARM1);
	float volume = G_FLOAT(OFS_PARM2);
	float attenuation = G_FLOAT(OFS_PARM3);
	SV_StartSound (sv.edicts, origin, 0, sample, volume, attenuation);
}

//file stuff

//returns false if the file is denied.
//fallbackread can be NULL, if the qc is not allowed to read that (original) file at all.
static qboolean QC_FixFileName(const char *name, const char **result, const char **fallbackread)
{
	if (!*name ||	//blank names are bad
		strchr(name, ':') ||	//dos/win absolute path, ntfs ADS, amiga drives. reject them all.
		strchr(name, '\\') ||	//windows-only paths.
		*name == '/' ||	//absolute path was given - reject
		strstr(name, ".."))	//someone tried to be clever.
	{
		return false;
	}

	*fallbackread = name;
	//if its a user config, ban any fallback locations so that csqc can't read passwords or whatever.
	if ((!strchr(name, '/') || q_strncasecmp(name, "configs/", 8)) 
		&& !q_strcasecmp(COM_FileGetExtension(name), "cfg")
		&& q_strncasecmp(name, "particles/", 10) && q_strncasecmp(name, "huds/", 5) && q_strncasecmp(name, "models/", 7))
		*fallbackread = NULL;
	*result = va("data/%s", name);
	return true;
}

//small note on access modes:
//when reading, we fopen files inside paks, for compat with (crappy non-zip-compatible) filesystem code
//when writing, we directly fopen the file such that it can never be inside a pak.
//this means that we need to take care when reading in order to detect EOF properly.
//writing doesn't need anything like that, so it can just dump stuff out, but we do need to ensure that the modes don't get mixed up, because trying to read from a writable file will not do what you would expect.
//even libc mandates a seek between reading+writing, so no great loss there.
static struct qcfile_s
{
	char cache[1024];
	int cacheoffset, cachesize;
	FILE *file;
	int fileoffset;
	int filesize;
	int filebase;	//the offset of the file inside a pak
	int mode;
} *qcfiles;
static size_t qcfiles_max;
#define QC_FILE_BASE 1
static void PF_fopen(void)
{
	const char *fname = G_STRING(OFS_PARM0);
	int fmode = G_FLOAT(OFS_PARM1);
	const char *fallback;
	FILE *file;
	size_t i;
	char name[MAX_OSPATH];
	int filesize = 0;

	G_FLOAT(OFS_RETURN) = -1;	//assume failure

	if (!QC_FixFileName(fname, &fname, &fallback))
	{
		Con_Printf("qcfopen: Access denied: %s\n", fname);
		return;
	}
	//if we were told to use 'foo.txt'
	//fname is now 'data/foo.txt'
	//fallback is now 'foo.txt', and must ONLY be read.

	switch(fmode)
	{
	case 0: //read
		filesize = COM_FOpenFile (fname, &file, NULL);
		if (!file && fallback)
			filesize = COM_FOpenFile (fallback, &file, NULL);
		break;
	case 1:	//append
		q_snprintf (name, sizeof(name), "%s/%s", com_gamedir, fname);
		Sys_mkdir (name);
		file = fopen(name, "w+b");
		if (file)
			fseek(file, 0, SEEK_END);
		break;
	case 2: //write
		q_snprintf (name, sizeof(name), "%s/%s", com_gamedir, fname);
		Sys_mkdir (name);
		file = fopen(name, "wb");
		break;
	}
	if (!file)
		return;

	for (i = 0; ; i++)
	{
		if (i == qcfiles_max)
		{
			qcfiles_max++;
			qcfiles = Z_Realloc(qcfiles, sizeof(*qcfiles)*qcfiles_max);
		}
		if (!qcfiles[i].file)
			break;
	}
	qcfiles[i].filebase = ftell(file);
	qcfiles[i].file = file;
	qcfiles[i].mode = fmode;
	//reading needs size info
	qcfiles[i].filesize = filesize;
	//clear the read cache.
	qcfiles[i].fileoffset = qcfiles[i].cacheoffset = qcfiles[i].cachesize = 0;

	G_FLOAT(OFS_RETURN) = i+QC_FILE_BASE;
}
static void PF_fgets(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	G_INT(OFS_RETURN) = 0;
	if (fileid >= qcfiles_max)
		Con_Warning("PF_fgets: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fgets: file not open\n");
	else if (qcfiles[fileid].mode != 0)
		Con_Warning("PF_fgets: file not open for reading\n");
	else
	{
		struct qcfile_s *f = &qcfiles[fileid];
		char *ret = PR_GetTempString();
		char *s = ret;
		char *end = ret+STRINGTEMP_LENGTH;
		for (;;)
		{
			if (!f->cachesize)
			{
				//figure out how much we can try to cache.
				int sz = f->filesize - f->fileoffset;
				if (sz < 0 || f->fileoffset < 0)	//... maybe we shouldn't have implemented seek support.
					sz = 0;
				else if ((size_t)sz > sizeof(f->cache))
					sz = sizeof(f->cache);
				//read a chunk
				f->cacheoffset = 0;
				f->cachesize = fread(f->cache, 1, sz, f->file);
				f->fileoffset += f->cachesize;
				if (!f->cachesize)
				{
					//classic eof...
					break;
				}
			}
			*s = f->cache[f->cacheoffset++];
			if (*s == '\n')	//new line, yay!
				break;
			s++;
			if (s == end)
				s--;	//rewind if we're overflowing, such that we truncate the string.
		}
		if (s > ret && s[-1] == '\r')
			s--;	//terminate it on the \r of a \r\n pair.
		*s = 0;	//terminate it
		G_INT(OFS_RETURN) = PR_SetEngineString(ret);
	}
}
static void PF_fputs(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	const char *str = PF_VarString(1);
	if (fileid >= qcfiles_max)
		Con_Warning("PF_fputs: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fputs: file not open\n");
	else if (qcfiles[fileid].mode == 0)
		Con_Warning("PF_fgets: file not open for writing\n");
	else
		fputs(str, qcfiles[fileid].file);
}
static void PF_fclose(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	if (fileid >= qcfiles_max)
		Con_Warning("PF_fclose: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fclose: file not open\n");
	else
	{
		fclose(qcfiles[fileid].file);
		qcfiles[fileid].file = NULL;
	}
}
static void PF_fseek(void)
{	//returns current position. or changes that position.
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	G_INT(OFS_RETURN) = 0;
	if (fileid >= qcfiles_max)
		Con_Warning("PF_fread: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fread: file not open\n");
	else
	{
		if (qcfiles[fileid].mode == 0)
			G_INT(OFS_RETURN) = qcfiles[fileid].fileoffset;	//when we're reading, use the cached read offset
		else
			G_INT(OFS_RETURN) = ftell(qcfiles[fileid].file)-qcfiles[fileid].filebase;
		if (pr_argc>1)
		{
			qcfiles[fileid].fileoffset = G_INT(OFS_PARM1);
			fseek(qcfiles[fileid].file, qcfiles[fileid].filebase+qcfiles[fileid].fileoffset, SEEK_SET);
			qcfiles[fileid].cachesize = qcfiles[fileid].cacheoffset = 0;
		}
	}
}
#if 0
static void PF_fread(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	int qcptr = G_INT(OFS_PARM1);
	size_t size = G_INT(OFS_PARM2);

	//FIXME: validate. make usable.
	char *nativeptr = (char*)sv.edicts + qcptr;
	
	G_INT(OFS_RETURN) = 0;
	if (fileid >= maxfiles)
		Con_Warning("PF_fread: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fread: file not open\n");
	else
		G_INT(OFS_RETURN) = fread(nativeptr, 1, size, qcfiles[fileid].file);
}
static void PF_fwrite(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	int qcptr = G_INT(OFS_PARM1);
	size_t size = G_INT(OFS_PARM2);

	//FIXME: validate. make usable.
	const char *nativeptr = (const char*)sv.edicts + qcptr;
	
	G_INT(OFS_RETURN) = 0;
	if (fileid >= maxfiles)
		Con_Warning("PF_fwrite: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fwrite: file not open\n");
	else
		G_INT(OFS_RETURN) = fwrite(nativeptr, 1, size, qcfiles[fileid].file);
}
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
static void PF_fsize(void)
{
	size_t fileid = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	G_INT(OFS_RETURN) = 0;
	if (fileid >= maxfiles)
		Con_Warning("PF_fread: invalid file handle\n");
	else if (!qcfiles[fileid].file)
		Con_Warning("PF_fread: file not open\n");
	else if (qcfiles[fileid].mode == 0)
	{
		G_INT(OFS_RETURN) = qcfiles[fileid].filesize;
		//can't truncate if we're reading.
	}
	else
	{
		long curpos = ftell(qcfiles[fileid].file);
		fseek(qcfiles[fileid].file, 0, SEEK_END);
		G_INT(OFS_RETURN) = ftell(qcfiles[fileid].file);
		fseek(qcfiles[fileid].file, curpos, SEEK_SET);

		if (pr_argc>1)
		{
			//specifically resize. or maybe extend.
#ifdef _WIN32
			_chsize(fileno(qcfiles[fileid].file), G_INT(OFS_PARM1));
#else
			ftruncate(fileno(qcfiles[fileid].file), G_INT(OFS_PARM1));
#endif
		}
	}
}
#endif


static void PF_search_begin(void)
{
//	const char *pattern = G_STRING(OFS_PARM0);
//	qboolean caseinsensitive = !!G_FLOAT(OFS_PARM0);
//	qboolaen quiet = !!G_FLOAT(OFS_PARM0);

	G_FLOAT(OFS_RETURN) = -1;
}
static void PF_search_end(void)
{
//	int handle = G_FLOAT(OFS_PARM0);
}
static void PF_search_getsize(void)
{
//	int handle = G_FLOAT(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = 0;
}
static void PF_search_getfilename(void)
{
//	int handle = G_FLOAT(OFS_PARM0);
//	int index = G_FLOAT(OFS_PARM1);
	G_INT(OFS_RETURN) = PR_SetEngineString("");
}
static void PF_search_getfilesize(void)
{
//	int handle = G_FLOAT(OFS_PARM0);
//	int index = G_FLOAT(OFS_PARM1);
	G_FLOAT(OFS_RETURN) = 0;
}
static void PF_search_getfilemtime(void)
{
//	int handle = G_FLOAT(OFS_PARM0);
//	int index = G_FLOAT(OFS_PARM1);
	G_INT(OFS_RETURN) = PR_SetEngineString("");
}

static void PF_whichpack(void)
{
	const char *fname = G_STRING(OFS_PARM0);	//uses native paths, as this isn't actually reading anything.
	unsigned int path_id;
	if (COM_FileExists(fname, &path_id))
	{
		//FIXME: quakespasm reports which gamedir the file is in, but paks are hidden.
		//I'm too lazy to rewrite COM_FindFile, so I'm just going to hack something small to get the gamedir, just not the paks

		searchpath_t *path;
		for (path = com_searchpaths; path; path = path->next)
			if (!path->pack && path->path_id == path_id)
				break;	//okay, this one looks like one we can report

		//sandbox it by stripping the basedir
		fname = path->filename;
		if (!strncmp(fname, com_basedir, strlen(com_basedir)))
			fname += strlen(com_basedir);
		else
			fname = "?";	//no idea where this came from. something is screwing with us.
		while (*fname == '/' || *fname == '\\')
			fname++;	//small cleanup, just in case
		G_INT(OFS_RETURN) = PR_SetEngineString(fname);
	}
	else
		G_INT(OFS_RETURN) = 0;
}

//string buffers

struct strbuf {
	qboolean isactive;
	char **strings;
	unsigned int used;
	unsigned int allocated;
};

#define BUFSTRBASE 1
#define NUMSTRINGBUFS 64u
struct strbuf strbuflist[NUMSTRINGBUFS];

static void PF_buf_shutdown(void)
{
	unsigned int i, bufno;

	for (bufno = 0; bufno < NUMSTRINGBUFS; bufno++)
	{
		if (!strbuflist[bufno].isactive)
			continue;

		for (i = 0; i < strbuflist[bufno].used; i++)
			Z_Free(strbuflist[bufno].strings[i]);
		Z_Free(strbuflist[bufno].strings);

		strbuflist[bufno].strings = NULL;
		strbuflist[bufno].used = 0;
		strbuflist[bufno].allocated = 0;
	}
}

// #440 float() buf_create (DP_QC_STRINGBUFFERS)
static void PF_buf_create(void)
{
	unsigned int i;

	const char *type = ((pr_argc>0)?G_STRING(OFS_PARM0):"string");
//	unsigned int flags = ((pr_argc>1)?G_FLOAT(OFS_PARM1):1);

	if (!q_strcasecmp(type, "string"))
		;
	else
	{
		G_FLOAT(OFS_RETURN) = -1;
		return;
	}

	//flags&1 == saved. apparently.

	for (i = 0; i < NUMSTRINGBUFS; i++)
	{
		if (!strbuflist[i].isactive)
		{
			strbuflist[i].isactive = true;
			strbuflist[i].used = 0;
			strbuflist[i].allocated = 0;
			strbuflist[i].strings = NULL;
			G_FLOAT(OFS_RETURN) = i+BUFSTRBASE;
			return;
		}
	}
	G_FLOAT(OFS_RETURN) = -1;
}
// #441 void(float bufhandle) buf_del (DP_QC_STRINGBUFFERS)
static void PF_buf_del(void)
{
	unsigned int i;
	unsigned int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;

	if (bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].isactive)
		return;

	for (i = 0; i < strbuflist[bufno].used; i++)
		Z_Free(strbuflist[bufno].strings[i]);
	Z_Free(strbuflist[bufno].strings);

	strbuflist[bufno].strings = NULL;
	strbuflist[bufno].used = 0;
	strbuflist[bufno].allocated = 0;

	strbuflist[bufno].isactive = false;
}
// #442 float(float bufhandle) buf_getsize (DP_QC_STRINGBUFFERS)
static void PF_buf_getsize(void)
{
	int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].isactive)
		return;

	G_FLOAT(OFS_RETURN) = strbuflist[bufno].used;
}
// #443 void(float bufhandle_from, float bufhandle_to) buf_copy (DP_QC_STRINGBUFFERS)
static void PF_buf_copy(void)
{
	unsigned int buffrom = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	unsigned int bufto = G_FLOAT(OFS_PARM1)-BUFSTRBASE;
	unsigned int i;

	if (bufto == buffrom)	//err...
		return;
	if (buffrom >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[buffrom].isactive)
		return;
	if (bufto >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufto].isactive)
		return;

	//obliterate any and all existing data.
	for (i = 0; i < strbuflist[bufto].used; i++)
		Z_Free(strbuflist[bufto].strings[i]);
	Z_Free(strbuflist[bufto].strings);

	//copy new data over.
	strbuflist[bufto].used = strbuflist[bufto].allocated = strbuflist[buffrom].used;
	strbuflist[bufto].strings = Z_Malloc(strbuflist[buffrom].used * sizeof(char*));
	for (i = 0; i < strbuflist[buffrom].used; i++)
		strbuflist[bufto].strings[i] = strbuflist[buffrom].strings[i]?Z_StrDup(strbuflist[buffrom].strings[i]):NULL;
}
static int PF_buf_sort_sortprefixlen;
static int PF_buf_sort_ascending(const void *a, const void *b)
{
	return strncmp(*(char**)a, *(char**)b, PF_buf_sort_sortprefixlen);
}
static int PF_buf_sort_descending(const void *b, const void *a)
{
	return strncmp(*(char**)a, *(char**)b, PF_buf_sort_sortprefixlen);
}
// #444 void(float bufhandle, float sortprefixlen, float backward) buf_sort (DP_QC_STRINGBUFFERS)
static void PF_buf_sort(void)
{
	int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	int sortprefixlen = G_FLOAT(OFS_PARM1);
	int backwards = G_FLOAT(OFS_PARM2);
	unsigned int s,d;
	char **strings;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].isactive)
		return;

	if (sortprefixlen <= 0)
		sortprefixlen = 0x7fffffff;

	//take out the nulls first, to avoid weird/crashy sorting
	for (s = 0, d = 0, strings = strbuflist[bufno].strings; s < strbuflist[bufno].used; )
	{
		if (!strings[s])
		{
			s++;
			continue;
		}
		strings[d++] = strings[s++];
	}
	strbuflist[bufno].used = d;

	//no nulls now, sort it.
	PF_buf_sort_sortprefixlen = sortprefixlen;	//eww, a global. burn in hell.
	if (backwards)	//z first
		qsort(strings, strbuflist[bufno].used, sizeof(char*), PF_buf_sort_descending);
	else	//a first
		qsort(strings, strbuflist[bufno].used, sizeof(char*), PF_buf_sort_ascending);
}
// #445 string(float bufhandle, string glue) buf_implode (DP_QC_STRINGBUFFERS)
static void PF_buf_implode(void)
{
	int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	const char *glue = G_STRING(OFS_PARM1);
	unsigned int gluelen = strlen(glue);
	unsigned int retlen, l, i;
	char **strings;
	char *ret;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].isactive)
		return;

	//count neededlength
	strings = strbuflist[bufno].strings;
	/*
	for (i = 0, retlen = 0; i < strbuflist[bufno].used; i++)
	{
		if (strings[i])
		{
			if (retlen)
				retlen += gluelen;
			retlen += strlen(strings[i]);
		}
	}
	ret = malloc(retlen+1);*/

	//generate the output
	ret = PR_GetTempString();
	for (i = 0, retlen = 0; i < strbuflist[bufno].used; i++)
	{
		if (strings[i])
		{
			if (retlen)
			{
				if (retlen+gluelen+1 > STRINGTEMP_LENGTH)
				{
					Con_Printf("PF_buf_implode: tempstring overflow\n");
					break;
				}
				memcpy(ret+retlen, glue, gluelen);
				retlen += gluelen;
			}
			l = strlen(strings[i]);
			if (retlen+l+1 > STRINGTEMP_LENGTH)
			{
				Con_Printf("PF_buf_implode: tempstring overflow\n");
				break;
			}
			memcpy(ret+retlen, strings[i], l);
			retlen += l;
		}
	}

	//add the null and return
	ret[retlen] = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(ret);
}
// #446 string(float bufhandle, float string_index) bufstr_get (DP_QC_STRINGBUFFERS)
static void PF_bufstr_get(void)
{
	unsigned int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	unsigned int index = G_FLOAT(OFS_PARM1);
	char *ret;

	if (bufno >= NUMSTRINGBUFS)
	{
		G_INT(OFS_RETURN) = 0;
		return;
	}
	if (!strbuflist[bufno].isactive)
	{
		G_INT(OFS_RETURN) = 0;
		return;
	}

	if (index >= strbuflist[bufno].used)
	{
		G_INT(OFS_RETURN) = 0;
		return;
	}

	ret = PR_GetTempString();
	q_strlcpy(ret, strbuflist[bufno].strings[index], STRINGTEMP_LENGTH);
	G_INT(OFS_RETURN) = PR_SetEngineString(ret);
}
// #447 void(float bufhandle, float string_index, string str) bufstr_set (DP_QC_STRINGBUFFERS)
static void PF_bufstr_set(void)
{
	unsigned int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	unsigned int index = G_FLOAT(OFS_PARM1);
	const char *string = G_STRING(OFS_PARM2);
	unsigned int oldcount;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].isactive)
		return;

	if (index >= strbuflist[bufno].allocated)
	{
		oldcount = strbuflist[bufno].allocated;
		strbuflist[bufno].allocated = (index + 256);
		strbuflist[bufno].strings = Z_Realloc(strbuflist[bufno].strings, strbuflist[bufno].allocated*sizeof(char*));
		memset(strbuflist[bufno].strings+oldcount, 0, (strbuflist[bufno].allocated - oldcount) * sizeof(char*));
	}
	if (strbuflist[bufno].strings[index])
		Z_Free(strbuflist[bufno].strings[index]);
	strbuflist[bufno].strings[index] = Z_Malloc(strlen(string)+1);
	strcpy(strbuflist[bufno].strings[index], string);

	if (index >= strbuflist[bufno].used)
		strbuflist[bufno].used = index+1;
}

static int PF_bufstr_add_internal(unsigned int bufno, const char *string, int appendonend)
{
	unsigned int index;
	if (appendonend)
	{
		//add on end
		index = strbuflist[bufno].used;
	}
	else
	{
		//find a hole
		for (index = 0; index < strbuflist[bufno].used; index++)
			if (!strbuflist[bufno].strings[index])
				break;
	}

	//expand it if needed
	if (index >= strbuflist[bufno].allocated)
	{
		unsigned int oldcount;
		oldcount = strbuflist[bufno].allocated;
		strbuflist[bufno].allocated = (index + 256);
		strbuflist[bufno].strings = Z_Realloc(strbuflist[bufno].strings, strbuflist[bufno].allocated*sizeof(char*));
		memset(strbuflist[bufno].strings+oldcount, 0, (strbuflist[bufno].allocated - oldcount) * sizeof(char*));
	}

	//add in the new string.
	if (strbuflist[bufno].strings[index])
		Z_Free(strbuflist[bufno].strings[index]);
	strbuflist[bufno].strings[index] = Z_Malloc(strlen(string)+1);
	strcpy(strbuflist[bufno].strings[index], string);

	if (index >= strbuflist[bufno].used)
		strbuflist[bufno].used = index+1;

	return index;
}

// #448 float(float bufhandle, string str, float order) bufstr_add (DP_QC_STRINGBUFFERS)
static void PF_bufstr_add(void)
{
	int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	const char *string = G_STRING(OFS_PARM1);
	int order = G_FLOAT(OFS_PARM2);

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].isactive)
		return;

	G_FLOAT(OFS_RETURN) = PF_bufstr_add_internal(bufno, string, order);
}
// #449 void(float bufhandle, float string_index) bufstr_free (DP_QC_STRINGBUFFERS)
static void PF_bufstr_free(void)
{
	unsigned int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	unsigned int index = G_FLOAT(OFS_PARM1);

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].isactive)
		return;

	if (index >= strbuflist[bufno].used)
		return;	//not valid anyway.

	if (strbuflist[bufno].strings[index])
		Z_Free(strbuflist[bufno].strings[index]);
	strbuflist[bufno].strings[index] = NULL;
}

/*static void PF_buf_cvarlist(void)
{
	int bufno = G_FLOAT(OFS_PARM0)-BUFSTRBASE;
	const char *pattern = G_STRING(OFS_PARM1);
	const char *antipattern = G_STRING(OFS_PARM2);
	int i;
	cvar_t	*var;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].isactive)
		return;

	//obliterate any and all existing data.
	for (i = 0; i < strbuflist[bufno].used; i++)
		Z_Free(strbuflist[bufno].strings[i]);
	Z_Free(strbuflist[bufno].strings);
	strbuflist[bufno].used = strbuflist[bufno].allocated = 0;

	//ignore name2, no point listing it twice.
	for (var=Cvar_FindVarAfter ("", CVAR_NONE) ; var ; var=var->next)
	{
		if (pattern && wildcmp(pattern, var->name))
			continue;
		if (antipattern && !wildcmp(antipattern, var->name))
			continue;

		PF_bufstr_add_internal(bufno, var->name, true);
	}
}*/

//directly reads a file into a stringbuffer
static void PF_buf_loadfile(void)
{
	const char *fname = G_STRING(OFS_PARM0);
	unsigned int bufno = G_FLOAT(OFS_PARM1)-BUFSTRBASE;
	char *line, *nl;
	const char *fallback;

	G_FLOAT(OFS_RETURN) = 0;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].isactive)
		return;

	if (!QC_FixFileName(fname, &fname, &fallback))
	{
		Con_Printf("qcfopen: Access denied: %s\n", fname);
		return;
	}
	line = (char*)COM_LoadTempFile(fname, NULL);
	if (!line && fallback)
		line = (char*)COM_LoadTempFile(fallback, NULL);
	if (!line)
		return;

	while(line)
	{
		nl = strchr(line, '\n');
		if (nl)
			*nl++ = 0;
		PF_bufstr_add_internal(bufno, line, true);
		line = nl;
	}

	G_FLOAT(OFS_RETURN) = true;
}

static void PF_buf_writefile(void)
{
	size_t fnum = G_FLOAT(OFS_PARM0) - QC_FILE_BASE;
	unsigned int bufno = G_FLOAT(OFS_PARM1)-BUFSTRBASE;
	char **strings;
	unsigned int idx, midx;

	G_FLOAT(OFS_RETURN) = 0;

	if ((unsigned int)bufno >= NUMSTRINGBUFS)
		return;
	if (!strbuflist[bufno].isactive)
		return;

	if (fnum >= qcfiles_max)
		return;
	if (!qcfiles[fnum].file)
		return;

	if (pr_argc >= 3)
	{
		if (G_FLOAT(OFS_PARM2) <= 0)
			idx = 0;
		else
			idx = G_FLOAT(OFS_PARM2);
	}
	else
		idx = 0;
	if (pr_argc >= 4)
		midx = idx + G_FLOAT(OFS_PARM3);
	else
		midx = strbuflist[bufno].used - idx;
	if (idx > strbuflist[bufno].used)
		idx = strbuflist[bufno].used;
	if (midx > strbuflist[bufno].used)
		midx = strbuflist[bufno].used;
	for(strings = strbuflist[bufno].strings; idx < midx; idx++)
	{
		if (strings[idx])
			fprintf(qcfiles[fnum].file, "%s\n", strings[idx]);
	}
	G_FLOAT(OFS_RETURN) = 1;
}

//entity stuff
static void PF_WasFreed(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = ed->free;
}
static void PF_copyentity(void)
{
	edict_t *src = G_EDICT(OFS_PARM0);
	edict_t *dst = G_EDICT(OFS_PARM1);
	if (src->free || dst->free)
		Con_Printf("PF_copyentity: entity is free\n");
	memcpy(&dst->v, &src->v, pr_edict_size - sizeof(edict_t));
	dst->alpha = src->alpha;
	dst->sendinterval = src->sendinterval;
	SV_LinkEdict(dst, false);
}
static void PF_edict_for_num(void)
{
	G_INT(OFS_RETURN) = EDICT_TO_PROG(EDICT_NUM(G_FLOAT(OFS_PARM0)));
}
static void PF_num_for_edict(void)
{
	G_FLOAT(OFS_RETURN) = G_EDICTNUM(OFS_PARM0);
}
static void PF_sv_findchain(void)
{
	edict_t	*ent, *chain;
	int	i, f;
	const char *s, *t;

	chain = (edict_t *)sv.edicts;

	f = G_INT(OFS_PARM0);
	s = G_STRING(OFS_PARM1);

	ent = NEXT_EDICT(sv.edicts);
	for (i = 1; i < sv.num_edicts; i++, ent = NEXT_EDICT(ent))
	{
		if (ent->free)
			continue;
		t = E_STRING(ent,f);
		if (strcmp(s, t))
			continue;
		ent->v.chain = EDICT_TO_PROG(chain);
		chain = ent;
	}

	RETURN_EDICT(chain);
}
static void PF_sv_findfloat(void)
{
	int		e;
	int		f;
	float	s, t;
	edict_t	*ed;

	e = G_EDICTNUM(OFS_PARM0);
	f = G_INT(OFS_PARM1);
	s = G_FLOAT(OFS_PARM2);

	for (e++ ; e < sv.num_edicts ; e++)
	{
		ed = EDICT_NUM(e);
		if (ed->free)
			continue;
		t = E_FLOAT(ed,f);
		if (t == s)
		{
			RETURN_EDICT(ed);
			return;
		}
	}

	RETURN_EDICT(sv.edicts);
}
static void PF_sv_findchainfloat(void)
{
	edict_t	*ent, *chain;
	int	i, f;
	float s, t;

	chain = (edict_t *)sv.edicts;

	f = G_INT(OFS_PARM0);
	s = G_FLOAT(OFS_PARM1);

	ent = NEXT_EDICT(sv.edicts);
	for (i = 1; i < sv.num_edicts; i++, ent = NEXT_EDICT(ent))
	{
		if (ent->free)
			continue;
		t = E_FLOAT(ent,f);
		if (s != t)
			continue;
		ent->v.chain = EDICT_TO_PROG(chain);
		chain = ent;
	}

	RETURN_EDICT(chain);
}
static void PF_sv_findflags(void)
{
	int		e;
	int		f;
	int		s, t;
	edict_t	*ed;

	e = G_EDICTNUM(OFS_PARM0);
	f = G_INT(OFS_PARM1);
	s = G_FLOAT(OFS_PARM2);

	for (e++ ; e < sv.num_edicts ; e++)
	{
		ed = EDICT_NUM(e);
		if (ed->free)
			continue;
		t = E_FLOAT(ed,f);
		if (t & s)
		{
			RETURN_EDICT(ed);
			return;
		}
	}

	RETURN_EDICT(sv.edicts);
}
static void PF_sv_findchainflags(void)
{
	edict_t	*ent, *chain;
	int	i, f;
	int s, t;

	chain = (edict_t *)sv.edicts;

	f = G_INT(OFS_PARM0);
	s = G_FLOAT(OFS_PARM1);

	ent = NEXT_EDICT(sv.edicts);
	for (i = 1; i < sv.num_edicts; i++, ent = NEXT_EDICT(ent))
	{
		if (ent->free)
			continue;
		t = E_FLOAT(ent,f);
		if (!(s & t))
			continue;
		ent->v.chain = EDICT_TO_PROG(chain);
		chain = ent;
	}

	RETURN_EDICT(chain);
}
static void PF_numentityfields(void)
{
	G_FLOAT(OFS_RETURN) = progs->numfielddefs;
}
static void PF_findentityfield(void)
{
	ddef_t *fld = ED_FindField(G_STRING(OFS_PARM0));
	if (fld)
		G_FLOAT(OFS_RETURN) = fld - pr_fielddefs;
	else
		G_FLOAT(OFS_RETURN) = 0;	//the first field is meant to be some dummy placeholder. or it could be modelindex...
}
static void PF_entityfieldref(void)
{
	unsigned int fldidx = G_FLOAT(OFS_PARM0);
	if (fldidx >= (unsigned int)progs->numfielddefs)
		G_INT(OFS_RETURN) = 0;
	else
		G_INT(OFS_RETURN) = pr_fielddefs[fldidx].ofs;
}
static void PF_entityfieldname(void)
{
	unsigned int fldidx = G_FLOAT(OFS_PARM0);
	if (fldidx < (unsigned int)progs->numfielddefs)
		G_INT(OFS_RETURN) = pr_fielddefs[fldidx].s_name;
	else
		G_INT(OFS_RETURN) = 0;
}
static void PF_entityfieldtype(void)
{
	unsigned int fldidx = G_FLOAT(OFS_PARM0);
	if (fldidx >= (unsigned int)progs->numfielddefs)
		G_INT(OFS_RETURN) = ev_void;
	else
		G_INT(OFS_RETURN) = pr_fielddefs[fldidx].type;
}
static void PF_getentityfieldstring(void)
{
	unsigned int fldidx = G_FLOAT(OFS_PARM0);
	edict_t *ent = G_EDICT(OFS_PARM1);
	if (fldidx < (unsigned int)progs->numfielddefs)
	{
		char *ret = PR_GetTempString();
		const char *val = PR_UglyValueString (pr_fielddefs[fldidx].type, (eval_t*)((float*)&ent->v + pr_fielddefs[fldidx].ofs));
		q_strlcpy(ret, val, STRINGTEMP_LENGTH);
		G_INT(OFS_RETURN) = PR_SetEngineString(ret);
	}
	else
		G_INT(OFS_RETURN) = 0;
}
static void PF_putentityfieldstring(void)
{
	unsigned int fldidx = G_FLOAT(OFS_PARM0);
	edict_t *ent = G_EDICT(OFS_PARM1);
	const char *value = G_STRING(OFS_PARM2);
	if (fldidx < (unsigned int)progs->numfielddefs)
		G_FLOAT(OFS_RETURN) = ED_ParseEpair ((void *)&ent->v, pr_fielddefs+fldidx, value);
	else
		G_FLOAT(OFS_RETURN) = false;
}
//static void PF_loadfromdata(void)
//{
//fixme;
//}
//static void PF_loadfromfile(void)
//{
//fixme;
//}

static void PF_parseentitydata(void)
{
	edict_t *ed = G_EDICT(OFS_PARM0);
	const char *data = G_STRING(OFS_PARM1), *end;
	unsigned int offset = (pr_argc>2)?G_FLOAT(OFS_PARM2):0;
	if (offset)
	{
		unsigned int len = strlen(data);
		if (offset > len)
			offset = len;
	}
	if (!data[offset])
		G_FLOAT(OFS_RETURN) = 0;
	else
	{
		end = ED_ParseEdict(data+offset, ed);
		G_FLOAT(OFS_RETURN) = end - data;
	}
}
static void PF_callfunction(void)
{
	dfunction_t *fnc;
	const char *fname;
	if (!pr_argc)
		return;
	pr_argc--;
	fname = G_STRING(OFS_PARM0 + pr_argc*3);
	fnc = ED_FindFunction(fname);
	if (fnc && fnc->first_statement > 0)
	{
		PR_ExecuteProgram(fnc - pr_functions);
	}
}
static void PF_isfunction(void)
{
	const char *fname = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = ED_FindFunction(fname)?true:false;
}

//other stuff
static void PF_gettime (void)
{
	int timer = (pr_argc > 0)?G_FLOAT(OFS_PARM0):0;
	switch(timer)
	{
	default:
		Con_DPrintf("PF_gettime: unsupported timer %i\n", timer);
	case 0:		//cached time at start of frame
		G_FLOAT(OFS_RETURN) = realtime;
		break;
	case 1:		//actual time
		G_FLOAT(OFS_RETURN) = Sys_DoubleTime();
		break;
	//case 2:	//highres.. looks like time into the frame. no idea
	//case 3:	//uptime
	//case 4:	//cd track
	//case 5:	//client simtime
	}
}
#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)
static void PF_infokey(void)
{
	unsigned int ent = G_EDICTNUM(OFS_PARM0);
	const char *key = G_STRING(OFS_PARM1);
	const char *r;
	char buf[64];
	if (!ent)
	{	//nq doesn't really do serverinfo. it just has some cvars.
		if (!strcmp(key, "*version"))
		{
			q_snprintf(buf, sizeof(buf), "QuakeSpasm %1.2f.%d"BUILD_SPECIAL_STR, (float)QUAKESPASM_VERSION, QUAKESPASM_VER_PATCH);
			r = buf;
		}
		else
		{
			cvar_t *var = Cvar_FindVar(key);
			if (var && (var->flags & CVAR_SERVERINFO))
				r = var->string;
			else
				r = NULL;
		}
	}
	else if (ent <= (unsigned int)svs.maxclients && svs.clients[ent-1].active)
	{
		ent--;
		r = buf;
		if (!strcmp(key, "ip"))
			r = NET_QSocketGetTrueAddressString(svs.clients[ent].netconnection);
		else if (!strcmp(key, "ping"))
		{
			float total = 0;
			unsigned int j;
			for (j = 0; j < NUM_PING_TIMES; j++)
				total+=svs.clients[ent].ping_times[j];
			total /= NUM_PING_TIMES;
			q_snprintf(buf, sizeof(buf), "%f", total);
		}
		else if (!strcmp(key, "protocol"))
		{
			switch(sv.protocol)
			{
			case PROTOCOL_NETQUAKE:
				r = "quake";
				break;
			case PROTOCOL_FITZQUAKE:
				r = "fitz666";
				break;
			case PROTOCOL_RMQ:
				r = "rmq999";
				break;
			default:
				r = "";
				break;
			}
		}
		else if (!strcmp(key, "name"))
			r = svs.clients[ent].name;
		else if (!strcmp(key, "topcolor"))
			q_snprintf(buf, sizeof(buf), "%u", svs.clients[ent].colors>>4);
		else if (!strcmp(key, "bottomcolor"))
			q_snprintf(buf, sizeof(buf), "%u", svs.clients[ent].colors&15);
		else if (!strcmp(key, "team"))	//nq doesn't really do teams. qw does though. yay compat?
			q_snprintf(buf, sizeof(buf), "t%u", (svs.clients[ent].colors&15)+1);
		else if (!strcmp(key, "*VIP"))
			r = "";
		else if (!strcmp(key, "*spectator"))
			r = "";
		else if (!strcmp(key, "skin"))
			r = "";
		else if (!strcmp(key, "csqcactive"))
			r = "0";
		else if (!strcmp(key, "rate"))
			r = "0";
		else
			r = NULL;
	}
	else r = NULL;

	if (r)
	{
		char *temp = PR_GetTempString();
		q_strlcpy(temp, r, STRINGTEMP_LENGTH);
		G_INT(OFS_RETURN) = PR_SetEngineString(temp);
	}
	else
		G_INT(OFS_RETURN) = 0;
}

static void PF_multicast_internal(qboolean reliable, byte *pvs, unsigned int requireext2)
{
	unsigned int i;
	int cluster;
	mleaf_t *playerleaf;
	if (!pvs)
	{
		if (!requireext2)
			SZ_Write((reliable?&sv.reliable_datagram:&sv.datagram), sv.multicast.data, sv.multicast.cursize);
		else
		{
			for (i = 0; i < (unsigned int)svs.maxclients; i++)
			{
				if (!svs.clients[i].active)
					continue;
				if (!(svs.clients[i].protocol_pext2 & requireext2))
					continue;
				SZ_Write((reliable?&svs.clients[i].message:&svs.clients[i].datagram), sv.multicast.data, sv.multicast.cursize);
			}
		}
	}
	else
	{
		for (i = 0; i < (unsigned int)svs.maxclients; i++)
		{
			if (!svs.clients[i].active)
				continue;

			if (requireext2 && !(svs.clients[i].protocol_pext2 & requireext2))
				continue;

			//figure out which cluster (read: pvs index) to use.
			playerleaf = Mod_PointInLeaf(svs.clients[i].edict->v.origin, sv.worldmodel);
			cluster = playerleaf - sv.worldmodel->leafs;
			cluster--;	//pvs is 1-based, leaf 0 is discarded.
			if (cluster < 0 || (pvs[cluster>>3] & (1<<(cluster&7))))
			{
				//they can see it. add it in to whichever buffer is appropriate.
				if (reliable)
					SZ_Write(&svs.clients[i].message, sv.multicast.data, sv.multicast.cursize);
				else
					SZ_Write(&svs.clients[i].datagram, sv.multicast.data, sv.multicast.cursize);
			}
		}
	}
}
//FIXME: shouldn't really be using pext2, but we don't track the earlier extensions, and it should be safe enough.
static void SV_Multicast(multicast_t to, float *org, int msg_entity, unsigned int requireext2)
{
	unsigned int i;

	if (to == MULTICAST_INIT && sv.state != ss_loading)
	{
		SZ_Write (&sv.signon, sv.multicast.data, sv.multicast.cursize);
		to = MULTICAST_ALL_R;	//and send to players that are already on
	}

	switch(to)
	{
	case MULTICAST_INIT:
		SZ_Write (&sv.signon, sv.multicast.data, sv.multicast.cursize);
		break;
	case MULTICAST_ALL_R:
	case MULTICAST_ALL_U:
		PF_multicast_internal(to==MULTICAST_PHS_R, NULL, requireext2);
		break;
	case MULTICAST_PHS_R:
	case MULTICAST_PHS_U:
		PF_multicast_internal(to==MULTICAST_PHS_R, NULL/*Mod_LeafPHS(Mod_PointInLeaf(org, sv.worldmodel), sv.worldmodel)*/, requireext2);	//we don't support phs, that would require lots of pvs decompression+merging stuff, and many q1bsps have a LOT of leafs.
		break;
	case MULTICAST_PVS_R:
	case MULTICAST_PVS_U:
		PF_multicast_internal(to==MULTICAST_PVS_R, Mod_LeafPVS(Mod_PointInLeaf(org, sv.worldmodel), sv.worldmodel), requireext2);
		break;
	case MULTICAST_ONE_R:
	case MULTICAST_ONE_U:
		i = msg_entity-1;
		if (i >= (unsigned int)svs.maxclients)
			break;
		//a unicast, which ignores pvs.
		//(unlike vanilla this allows unicast unreliables, so woo)
		if (svs.clients[i].active)
		{
			SZ_Write(((to==MULTICAST_ONE_R)?&svs.clients[i].message:&svs.clients[i].datagram), sv.multicast.data, sv.multicast.cursize);
		}
		break;
	default:
		break;
	}
	SZ_Clear(&sv.multicast);
}
static void PF_multicast(void)
{
	float *org = G_VECTOR(OFS_PARM0);
	multicast_t to = G_FLOAT(OFS_PARM1);
	SV_Multicast(to, org, 0, 0);
}
static void PF_randomvector(void)
{
	vec3_t temp;
	do
	{
		temp[0] = (rand() & 32767) * (2.0 / 32767.0) - 1.0;
		temp[1] = (rand() & 32767) * (2.0 / 32767.0) - 1.0;
		temp[2] = (rand() & 32767) * (2.0 / 32767.0) - 1.0;
	} while (DotProduct(temp, temp) >= 1);
	VectorCopy (temp, G_VECTOR(OFS_RETURN));
}
static void PF_checkextension(void);
static void PF_checkbuiltin(void);
static void PF_builtinsupported(void);

static void PF_uri_escape(void)
{
	static const char *hex = "0123456789ABCDEF";

	char *result = PR_GetTempString();
	char *o = result;
	const unsigned char *s = (const unsigned char*)G_STRING(OFS_PARM0);
	*result = 0;
	while (*s && o < result+STRINGTEMP_LENGTH-4)
	{
		if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')
				|| *s == '.' || *s == '-' || *s == '_')
			*o++ = *s++;
		else
		{
			*o++ = '%';
			*o++ = hex[*s>>4];
			*o++ = hex[*s&0xf];
			s++;
		}
	}
	*o = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(result);
}
static void PF_uri_unescape(void)
{
	const char *s = G_STRING(OFS_PARM0), *i;
	char *resultbuf = PR_GetTempString(), *o;
	unsigned char hex;
	i = s; o = resultbuf;
	while (*i && o < resultbuf+STRINGTEMP_LENGTH-2)
	{
		if (*i == '%')
		{
			hex = 0;
			if (i[1] >= 'A' && i[1] <= 'F')
				hex += i[1]-'A'+10;
			else if (i[1] >= 'a' && i[1] <= 'f')
				hex += i[1]-'a'+10;
			else if (i[1] >= '0' && i[1] <= '9')
				hex += i[1]-'0';
			else
			{
				*o++ = *i++;
				continue;
			}
			hex <<= 4;
			if (i[2] >= 'A' && i[2] <= 'F')
				hex += i[2]-'A'+10;
			else if (i[2] >= 'a' && i[2] <= 'f')
				hex += i[2]-'a'+10;
			else if (i[2] >= '0' && i[2] <= '9')
				hex += i[2]-'0';
			else
			{
				*o++ = *i++;
				continue;
			}
			*o++ = hex;
			i += 3;
		}
		else
			*o++ = *i++;
	}
	*o = 0;
	G_INT(OFS_RETURN) = PR_SetEngineString(resultbuf);
}
static void PF_crc16(void)
{
	qboolean insens = G_FLOAT(OFS_PARM0);
	const char *str = PF_VarString(1);
	size_t len = strlen(str);

	if (insens)
	{
		unsigned short	crc;

		CRC_Init (&crc);
		while (len--)
			CRC_ProcessByte(&crc, q_tolower(*str++));
		G_FLOAT(OFS_RETURN) = crc;
	}
	else
		G_FLOAT(OFS_RETURN) = CRC_Block((byte*)str, len);
}

static void PF_strlennocol(void)
{
	//quakespasm doesn't support colour codes. that makes this a bit of a no-op.
	//there's no single set either, so this stuff is a bit awkward in ssqc. at least nothing will crash.
	G_FLOAT(OFS_RETURN) = strlen(G_STRING(OFS_PARM0));
}
static void PF_strdecolorize(void)
{
	//quakespasm doesn't support colour codes. that makes this a bit of a no-op.
	//there's no single set either, so this stuff is a bit awkward in ssqc. at least nothing will crash.
	G_INT(OFS_RETURN) = G_INT(OFS_PARM0);
}
static void PF_setattachment(void)
{
	edict_t *ent = G_EDICT(OFS_PARM0);
	edict_t *tagent = G_EDICT(OFS_PARM1);
	const char *tagname = G_STRING(OFS_PARM2);
	eval_t *val;

	if (*tagname)
	{
		//we don't support md3s, or any skeletal formats, so all tag names are logically invalid for us.
		Con_DWarning("PF_setattachment: tag %s not found\n", tagname);
	}

	if ((val = GetEdictFieldValue(ent, pr_extfields.tag_entity)))
		val->edict = EDICT_TO_PROG(tagent);
	if ((val = GetEdictFieldValue(ent, pr_extfields.tag_index)))
		val->_float = 0;
}
static void PF_void_stub(void)
{
	G_FLOAT(OFS_RETURN) = 0;
}

#ifdef PSET_SCRIPT
int PF_SV_ForceParticlePrecache(const char *s)
{
	unsigned int i;
	for (i = 1; i < MAX_PARTICLETYPES; i++)
	{
		if (!sv.particle_precache[i])
		{
			if (sv.state != ss_loading)
			{
				MSG_WriteByte(&sv.multicast, svcdp_precache);
				MSG_WriteShort(&sv.multicast, i|0x4000);
				MSG_WriteString(&sv.multicast, s);
				SV_Multicast(MULTICAST_ALL_R, NULL, 0, PEXT2_REPLACEMENTDELTAS); //FIXME
			}

			sv.particle_precache[i] = strcpy(Hunk_Alloc(strlen(s)+1), s);	//weirdness to avoid issues with tempstrings
			return i;
		}
		if (!strcmp(sv.particle_precache[i], s))
			return i;
	}
	return 0;
}
static void PF_sv_particleeffectnum(void)
{
	const char	*s;
	unsigned int i;
#ifdef PSET_SCRIPT
	extern cvar_t r_particledesc;
#endif

	s = G_STRING(OFS_PARM0);
	G_FLOAT(OFS_RETURN) = 0;
//	PR_CheckEmptyString (s);

	if (!*s)
		return;

#ifdef PSET_SCRIPT
	if (!sv.particle_precache[1] && (!strncmp(s, "effectinfo.", 11) || strstr(r_particledesc.string, "effectinfo")))
		COM_Effectinfo_Enumerate(PF_SV_ForceParticlePrecache);
#endif

	for (i = 1; i < MAX_PARTICLETYPES; i++)
	{
		if (!sv.particle_precache[i])
		{
			if (sv.state != ss_loading)
			{
				Con_Warning ("PF_sv_particleeffectnum(%s): Precache should only be done in spawn functions\n", s);

				MSG_WriteByte(&sv.multicast, svcdp_precache);
				MSG_WriteShort(&sv.multicast, i|0x4000);
				MSG_WriteString(&sv.multicast, s);
				SV_Multicast(MULTICAST_ALL_R, NULL, 0, PEXT2_REPLACEMENTDELTAS);
			}

			sv.particle_precache[i] = strcpy(Hunk_Alloc(strlen(s)+1), s);	//weirdness to avoid issues with tempstrings
			G_FLOAT(OFS_RETURN) = i;
			return;
		}
		if (!strcmp(sv.particle_precache[i], s))
		{
			if (sv.state != ss_loading && !pr_checkextension.value)
				Con_Warning ("PF_sv_particleeffectnum(%s): Precache should only be done in spawn functions\n", s);
			G_FLOAT(OFS_RETURN) = i;
			return;
		}
	}
	PR_RunError ("PF_sv_particleeffectnum: overflow");
}
static void PF_sv_trailparticles(void)
{
	int efnum;
	int ednum;
	float *start = G_VECTOR(OFS_PARM2);
	float *end = G_VECTOR(OFS_PARM3);

	/*DP gets this wrong, lets try to be compatible*/
	if (G_INT(OFS_PARM1) >= MAX_EDICTS)
	{
		ednum = G_EDICTNUM(OFS_PARM0);
		efnum = G_FLOAT(OFS_PARM1);
	}
	else
	{
		efnum = G_FLOAT(OFS_PARM0);
		ednum = G_EDICTNUM(OFS_PARM1);
	}

	if (efnum <= 0)
		return;

	MSG_WriteByte(&sv.multicast, svcdp_trailparticles);
	MSG_WriteShort(&sv.multicast, ednum);
	MSG_WriteShort(&sv.multicast, efnum);
	MSG_WriteCoord(&sv.multicast, start[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, start[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, start[2], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[0], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[1], sv.protocolflags);
	MSG_WriteCoord(&sv.multicast, end[2], sv.protocolflags);

	SV_Multicast(MULTICAST_PHS_U, start, 0, PEXT2_REPLACEMENTDELTAS);
}
static void PF_sv_pointparticles(void)
{
	int efnum = G_FLOAT(OFS_PARM0);
	float *org = G_VECTOR(OFS_PARM1);
	float *vel = (pr_argc < 3)?vec3_origin:G_VECTOR(OFS_PARM2);
	int count = (pr_argc < 4)?1:G_FLOAT(OFS_PARM3);

	if (efnum <= 0)
		return;
	if (count > 65535)
		count = 65535;
	if (count < 1)
		return;

	if (count == 1 && !vel[0] && !vel[1] && !vel[2])
	{
		MSG_WriteByte(&sv.multicast, svcdp_pointparticles1);
		MSG_WriteShort(&sv.multicast, efnum);
		MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
	}
	else
	{
		MSG_WriteByte(&sv.multicast, svcdp_pointparticles);
		MSG_WriteShort(&sv.multicast, efnum);
		MSG_WriteCoord(&sv.multicast, org[0], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, org[1], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, org[2], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, vel[0], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, vel[1], sv.protocolflags);
		MSG_WriteCoord(&sv.multicast, vel[2], sv.protocolflags);
		MSG_WriteShort(&sv.multicast, count);
	}

	SV_Multicast(MULTICAST_PHS_U, org, 0, PEXT2_REPLACEMENTDELTAS);
}
#else
#define PF_sv_particleeffectnum PF_void_stub
#define PF_sv_trailparticles PF_void_stub
#define PF_sv_pointparticles PF_void_stub
#endif

//A quick note on number ranges.
//0: automatically assigned. more complicated, but no conflicts over numbers, just names...
//   NOTE: #0 is potentially ambiguous - vanilla will interpret it as instruction 0 (which is normally reserved) rather than a builtin.
//         if such functions were actually used, this would cause any 64bit engines that switched to unsigned types to crash due to an underflow.
//         we do some sneaky hacks to avoid changes to the vm... because we're evil.
//0-199: free for all.
//200-299: fte's random crap
//300-399: csqc's random crap
//400+: dp's random crap
static struct
{
	const char *name;
	builtin_t func;
	int documentednumber;
	const char *typestr;
	const char *desc;
	int number;
} extensionbuiltins[] = 
{
	{"vectoangles2",	PF_ext_vectoangles,	51,	D("vector(vector fwd, optional vector up)", "Returns the angles (+x=UP) required to orient an entity to look in the given direction. The 'up' argument is required if you wish to set a roll angle, otherwise it will be limited to just monster-style turning.")},

	{"sin",				PF_Sin,				60,	"float(float angle)"},	//60
	{"cos",				PF_Cos,				61,	"float(float angle)"},	//61
	{"sqrt",			PF_Sqrt,			62,	"float(float value)"},	//62
	{"tracetoss",		PF_TraceToss,		64,	"void(entity ent, entity ignore)"},
	{"etos",			PF_etos,			65,	"string(entity ent)"},



	{"infokey",			PF_infokey,			80, D("string(entity e, string key)", "If e is world, returns the field 'key' from either the serverinfo or the localinfo. If e is a player, returns the value of 'key' from the player's userinfo string. There are a few special exceptions, like 'ip' which is not technically part of the userinfo.")},	//80
	{"stof",			PF_stof,			81,	"float(string)"},	//81
	{"multicast",		PF_multicast,		82,	D("#define unicast(pl,reli) do{msg_entity = pl; multicast('0 0 0', reli?MULITCAST_ONE_R:MULTICAST_ONE);}while(0)\n"
																		"void(vector where, float set)", "Once the MSG_MULTICAST network message buffer has been filled with data, this builtin is used to dispatch it to the given target, filtering by pvs for reduced network bandwidth.")},	//82
	{"tracebox",		PF_tracebox,		90,	D("void(vector start, vector mins, vector maxs, vector end, float nomonsters, entity ent)", "Exactly like traceline, but a box instead of a uselessly thin point. Acceptable sizes are limited by bsp format, q1bsp has strict acceptable size values.")},
	{"randomvec",		PF_randomvector,	91,	D("vector()", "Returns a vector with random values. Each axis is independantly a value between -1 and 1 inclusive.")},
	{"getlight",		PF_sv_getlight,		92, "vector(vector org)"},// (DP_QC_GETLIGHT),
	{"registercvar",	PF_registercvar,	93,	D("float(string cvarname, string defaultvalue)", "Creates a new cvar on the fly. If it does not already exist, it will be given the specified value. If it does exist, this is a no-op.\nThis builtin has the limitation that it does not apply to configs or commandlines. Such configs will need to use the set or seta command causing this builtin to be a noop.\nIn engines that support it, you will generally find the autocvar feature easier and more efficient to use.")},
	{"min",				PF_min,				94,	D("float(float a, float b, ...)", "Returns the lowest value of its arguments.")},// (DP_QC_MINMAXBOUND)
	{"max",				PF_max,				95,	D("float(float a, float b, ...)", "Returns the highest value of its arguments.")},// (DP_QC_MINMAXBOUND)
	{"bound",			PF_bound,			96,	D("float(float minimum, float val, float maximum)", "Returns val, unless minimum is higher, or maximum is less.")},// (DP_QC_MINMAXBOUND)
	{"pow",				PF_pow,				97,	"float(float value, float exp)"},
	{"findfloat",		PF_sv_findfloat,	98, D("#define findentity findfloat\nentity(entity start, .__variant fld, __variant match)", "Equivelent to the find builtin, but instead of comparing strings contents, this builtin compares the raw values. This builtin requires multiple calls in order to scan all entities - set start to the previous call's return value.\nworld is returned when there are no more entities.")},	// #98 (DP_QC_FINDFLOAT)
	{"checkextension",	PF_checkextension,	99,	D("float(string extname)", "Checks for an extension by its name (eg: checkextension(\"FRIK_FILE\") says that its okay to go ahead and use strcat).\nUse cvar(\"pr_checkextension\") to see if this builtin exists.")},	// #99	//darkplaces system - query a string to see if the mod supports X Y and Z.
	{"checkbuiltin",	PF_checkbuiltin,	0,	D("float(__variant funcref)", "Checks to see if the specified builtin is supported/mapped. This is intended as a way to check for #0 functions, allowing for simple single-builtin functions.")},
	{"builtin_find",	PF_builtinsupported,100,D("float(string builtinname)", "Looks to see if the named builtin is valid, and returns the builtin number it exists at.")},	// #100	//per builtin system.
	{"anglemod",		PF_anglemod,		102,"float(float value)"},

	{"fopen",			PF_fopen,			110, D("filestream(string filename, float mode, optional float mmapminsize)", "Opens a file, typically prefixed with \"data/\", for either read or write access.")},	// (FRIK_FILE)
	{"fclose",			PF_fclose,			111, "void(filestream fhandle)"},	// (FRIK_FILE)
	{"fgets",			PF_fgets,			112, D("string(filestream fhandle)", "Reads a single line out of the file. The new line character is not returned as part of the string. Returns the null string on EOF (use if not(string) to easily test for this, which distinguishes it from the empty string which is returned if the line being read is blank")},	// (FRIK_FILE)
	{"fputs",			PF_fputs,			113, D("void(filestream fhandle, string s, optional string s2, optional string s3, optional string s4, optional string s5, optional string s6, optional string s7)", "Writes the given string(s) into the file. For compatibility with fgets, you should ensure that the string is terminated with a \\n - this will not otherwise be done for you. It is up to the engine whether dos or unix line endings are actually written.")},	// (FRIK_FILE)
//		{"fread",			PF_fread,			0,	 D("int(filestream fhandle, void *ptr, int size)", "Reads binary data out of the file. Returns truncated lengths if the read exceeds the length of the file.")},
//		{"fwrite",			PF_fwrite,			0,	 D("int(filestream fhandle, void *ptr, int size)", "Writes binary data out of the file.")},
		{"fseek",			PF_fseek,			0,	 D("#define ftell fseek //c-compat\nint(filestream fhandle, optional int newoffset)", "Changes the current position of the file, if specified. Returns prior position, in bytes.")},
//		{"fsize",			PF_fsize,			0,	 D("int(filestream fhandle, optional int newsize)", "Reports the total size of the file, in bytes. Can also be used to truncate/extend the file")},
	{"strlen",			PF_strlen,			114, "float(string s)"},	// (FRIK_FILE)
	{"strcat",			PF_strcat,			115, "string(string s1, optional string s2, optional string s3, optional string s4, optional string s5, optional string s6, optional string s7, optional string s8)"},	// (FRIK_FILE)
	{"substring",		PF_substring,		116, "string(string s, float start, float length)"},	// (FRIK_FILE)
	{"stov",			PF_stov,			117, "vector(string s)"},	// (FRIK_FILE)
	{"strzone",			PF_strzone,			118, D("string(string s, ...)", "Create a semi-permanent copy of a string that only becomes invalid once strunzone is called on the string (instead of when the engine assumes your string has left scope).")},	// (FRIK_FILE)
	{"strunzone",		PF_strunzone,		119, D("void(string s)", "Destroys a string that was allocated by strunzone. Further references to the string MAY crash the game.")},	// (FRIK_FILE)

	{"bitshift",		PF_bitshift,		218,	"float(float number, float quantity)"},
	{"te_lightningblood",PF_sv_te_lightningblood,219,"void(vector org)"},
	{"strstrofs",		PF_strstrofs,		221,	D("float(string s1, string sub, optional float startidx)", "Returns the 0-based offset of sub within the s1 string, or -1 if sub is not in s1.\nIf startidx is set, this builtin will ignore matches before that 0-based offset.")},
	{"str2chr",			PF_str2chr,			222,	D("float(string str, float index)", "Retrieves the character value at offset 'index'.")},
	{"chr2str",			PF_chr2str,			223,	D("string(float chr, ...)", "The input floats are considered character values, and are concatenated.")},
	{"strconv",			PF_strconv,			224,	D("string(float ccase, float redalpha, float redchars, string str, ...)", "Converts quake chars in the input string amongst different representations.\nccase specifies the new case for letters.\n 0: not changed.\n 1: forced to lower case.\n 2: forced to upper case.\nredalpha and redchars switch between colour ranges.\n 0: no change.\n 1: Forced white.\n 2: Forced red.\n 3: Forced gold(low) (numbers only).\n 4: Forced gold (high) (numbers only).\n 5+6: Forced to white and red alternately.\nYou should not use this builtin in combination with UTF-8.")},
	{"strpad",			PF_strpad,			225,	D("string(float pad, string str1, ...)", "Pads the string with spaces, to ensure its a specific length (so long as a fixed-width font is used, anyway). If pad is negative, the spaces are added on the left. If positive the padding is on the right.")},	//will be moved
	{"infoadd",			PF_infoadd,			226,	D("string(infostring old, string key, string value)", "Returns a new tempstring infostring with the named value changed (or added if it was previously unspecified). Key and value may not contain the \\ character.")},
	{"infoget",			PF_infoget,			227,	D("string(infostring info, string key)", "Reads a named value from an infostring. The returned value is a tempstring")},
//	{"strcmp",			PF_strncmp,			228,	D("float(string s1, string s2)", "Compares the two strings exactly. s1ofs allows you to treat s2 as a substring to compare against, or should be 0.\nReturns 0 if the two strings are equal, a negative value if s1 appears numerically lower, and positive if s1 appears numerically higher.")},
	{"strncmp",			PF_strncmp,			228,	D("#define strcmp strncmp\nfloat(string s1, string s2, optional float len, optional float s1ofs, optional float s2ofs)", "Compares up to 'len' chars in the two strings. s1ofs allows you to treat s2 as a substring to compare against, or should be 0.\nReturns 0 if the two strings are equal, a negative value if s1 appears numerically lower, and positive if s1 appears numerically higher.")},
	{"strcasecmp",		PF_strncasecmp,		229,	D("float(string s1, string s2)",  "Compares the two strings without case sensitivity.\nReturns 0 if they are equal. The sign of the return value may be significant, but should not be depended upon.")},
	{"strncasecmp",		PF_strncasecmp,		230,	D("float(string s1, string s2, float len, optional float s1ofs, optional float s2ofs)", "Compares up to 'len' chars in the two strings without case sensitivity. s1ofs allows you to treat s2 as a substring to compare against, or should be 0.\nReturns 0 if they are equal. The sign of the return value may be significant, but should not be depended upon.")},
	{"strtrim",			PF_strtrim,			0,		D("string(string s)", "Trims the whitespace from the start+end of the string.")},
	{"te_bloodqw",		PF_sv_te_bloodqw,	239,	"void(vector org, float count)"},
	{"mod",				PF_mod,				245,	"float(float a, float n)"},
	{"stoi",			PF_stoi,			259,	D("int(string)", "Converts the given string into a true integer. Base 8, 10, or 16 is determined based upon the format of the string.")},
	{"itos",			PF_itos,			260,	D("string(int)", "Converts the passed true integer into a base10 string.")},
	{"stoh",			PF_stoh,			261,	D("int(string)", "Reads a base-16 string (with or without 0x prefix) as an integer. Bugs out if given a base 8 or base 10 string. :P")},
	{"htos",			PF_htos,			262,	D("string(int)", "Formats an integer as a base16 string, with leading 0s and no prefix. Always returns 8 characters.")},
	{"ftoi",			PF_ftoi,			0,		D("int(float)", "Converts the given float into a true integer without depending on extended qcvm instructions.")},
	{"itof",			PF_itof,			0,		D("float(int)", "Converts the given true integer into a float without depending on extended qcvm instructions.")},
	{"crossproduct",	PF_crossproduct,	0,		D("#define dotproduct(v1,v2) ((vector)(v1)*(vector)(v2))\nvector(vector v1, vector v2)", "Small helper function to calculate the crossproduct of two vectors.")},
	{"frameforname",	PF_frameforname,	276,	D("float(float modidx, string framename)", "Looks up a framegroup from a model by name, avoiding the need for hardcoding. Returns -1 on error.")},// (FTE_CSQC_SKELETONOBJECTS)
	{"frameduration",	PF_frameduration,	277,	D("float(float modidx, float framenum)", "Retrieves the duration (in seconds) of the specified framegroup.")},// (FTE_CSQC_SKELETONOBJECTS)
	{"WriteFloat",		PF_WriteFloat,		280,	"void(float buf, float fl)"},
	{"frametoname",		PF_frametoname,		284,	"string(float modidx, float framenum)"},
	{"checkcommand",	PF_checkcommand,	294,	D("float(string name)", "Checks to see if the supplied name is a valid command, cvar, or alias. Returns 0 if it does not exist.")},
	{"particleeffectnum",PF_sv_particleeffectnum,335,D("float(string effectname)", "Precaches the named particle effect. If your effect name is of the form 'foo.bar' then particles/foo.cfg will be loaded by the client if foo.bar was not already defined.\nDifferent engines will have different particle systems, this specifies the QC API only.")},// (EXT_CSQC)
	{"trailparticles",	PF_sv_trailparticles,336,	D("void(float effectnum, entity ent, vector start, vector end)", "Draws the given effect between the two named points. If ent is not world, distances will be cached in the entity in order to avoid framerate dependancies. The entity is not otherwise used.")},// (EXT_CSQC),
	{"pointparticles",	PF_sv_pointparticles,337,	D("void(float effectnum, vector origin, optional vector dir, optional float count)", "Spawn a load of particles from the given effect at the given point traveling or aiming along the direction specified. The number of particles are scaled by the count argument.")},// (EXT_CSQC)
	{"print",			PF_print,			339,	D("void(string s, ...)", "Unconditionally print on the local system's console, even in ssqc (doesn't care about the value of the developer cvar).")},//(EXT_CSQC)
	{"wasfreed",		PF_WasFreed,		353,	D("float(entity ent)", "Quickly check to see if the entity is currently free. This function is only valid during the two-second non-reuse window, after that it may give bad results. Try one second to make it more robust.")},//(EXT_CSQC) (should be availabe on server too)

	{"copyentity",		PF_copyentity,		400,	D("entity(entity from, optional entity to)", "Copies all fields from one entity to another.")},// (DP_QC_COPYENTITY)
	{"setcolors",		PF_setcolors,		401,	D("void(entity ent, float colours)", "Changes a player's colours. The bits 0-3 are the lower/trouser colour, bits 4-7 are the upper/shirt colours.")},//DP_SV_SETCOLOR
	{"findchain",		PF_sv_findchain,	402,	"entity(.string field, string match)"},// (DP_QC_FINDCHAIN)
	{"findchainfloat",	PF_sv_findchainfloat,403,	"entity(.float fld, float match)"},// (DP_QC_FINDCHAINFLOAT)
		{"effect",			PF_sv_effect,			404,	D("void(vector org, string modelname, float startframe, float endframe, float framerate)", "stub. Spawns a self-animating sprite")},// (DP_SV_EFFECT)
	{"te_blood",		PF_sv_te_blooddp,	405,	"void(vector org, vector dir, float count)"},// #405 te_blood
		{"te_bloodshower",	PF_sv_te_bloodshower,	406,	"void(vector mincorner, vector maxcorner, float explosionspeed, float howmany)", "stub."},// (DP_TE_BLOODSHOWER)
		{"te_explosionrgb",	PF_sv_te_explosionrgb,	407,	"void(vector org, vector color)", "stub."},// (DP_TE_EXPLOSIONRGB)
		{"te_particlecube",	PF_sv_te_particlecube,	408,	"void(vector mincorner, vector maxcorner, vector vel, float howmany, float color, float gravityflag, float randomveljitter)", "stub."},// (DP_TE_PARTICLECUBE)
	{"te_particlerain",	PF_sv_te_particlerain,	409,	"void(vector mincorner, vector maxcorner, vector vel, float howmany, float color)"},// (DP_TE_PARTICLERAIN)
	{"te_particlesnow",	PF_sv_te_particlesnow,	410,	"void(vector mincorner, vector maxcorner, vector vel, float howmany, float color)"},// (DP_TE_PARTICLESNOW)
		{"te_spark",		PF_sv_te_spark,		411,	"void(vector org, vector vel, float howmany)", "stub."},// (DP_TE_SPARK)
		{"te_gunshotquad",	PF_sv_te_gunshotquad,	412,	"void(vector org)", "stub."},// (DP_TE_QUADEFFECTS1)
		{"te_spikequad",	PF_sv_te_spikequad,	413,	"void(vector org)", "stub."},// (DP_TE_QUADEFFECTS1)
		{"te_superspikequad",PF_sv_te_superspikequad,414,	"void(vector org)", "stub."},// (DP_TE_QUADEFFECTS1)
		{"te_explosionquad",PF_sv_te_explosionquad,415,	"void(vector org)", "stub."},// (DP_TE_QUADEFFECTS1)
		{"te_smallflash",	PF_sv_te_smallflash,	416,	"void(vector org)", "stub."},// (DP_TE_SMALLFLASH)
		{"te_customflash",	PF_sv_te_customflash,	417,	"void(vector org, float radius, float lifetime, vector color)", "stub."},// (DP_TE_CUSTOMFLASH)
	{"te_gunshot",		PF_sv_te_gunshot,	418,	"void(vector org, optional float count)"},// #418 te_gunshot
	{"te_spike",		PF_sv_te_spike,		419,	"void(vector org)"},// #419 te_spike
	{"te_superspike",	PF_sv_te_superspike,420,	"void(vector org)"},// #420 te_superspike
	{"te_explosion",	PF_sv_te_explosion,	421,	"void(vector org)"},// #421 te_explosion
	{"te_tarexplosion",	PF_sv_te_tarexplosion,422,	"void(vector org)"},// #422 te_tarexplosion
	{"te_wizspike",		PF_sv_te_wizspike,	423,	"void(vector org)"},// #423 te_wizspike
	{"te_knightspike",	PF_sv_te_knightspike,424,	"void(vector org)"},// #424 te_knightspike
	{"te_lavasplash",	PF_sv_te_lavasplash,425,	"void(vector org)"},// #425 te_lavasplash
	{"te_teleport",		PF_sv_te_teleport,	426,	"void(vector org)"},// #426 te_teleport
	{"te_explosion2",	PF_sv_te_explosion2,427,	"void(vector org, float color, float colorlength)"},// #427 te_explosion2
	{"te_lightning1",	PF_sv_te_lightning1,428,	"void(entity own, vector start, vector end)"},// #428 te_lightning1
	{"te_lightning2",	PF_sv_te_lightning2,429,	"void(entity own, vector start, vector end)"},// #429 te_lightning2
	{"te_lightning3",	PF_sv_te_lightning3,430,	"void(entity own, vector start, vector end)"},// #430 te_lightning3
	{"te_beam",			PF_sv_te_beam,		431,	"void(entity own, vector start, vector end)"},// #431 te_beam
	{"vectorvectors",	PF_vectorvectors,	432,	"void(vector dir)"},// (DP_QC_VECTORVECTORS)
		{"te_plasmaburn",	PF_sv_te_plasmaburn,433,	"void(vector org)", "stub."},// (DP_TE_PLASMABURN)
	{"getsurfacenumpoints",PF_getsurfacenumpoints,434,	"float(entity e, float s)"},// (DP_QC_GETSURFACE)
	{"getsurfacepoint",PF_getsurfacepoint,	435,	"vector(entity e, float s, float n)"},// (DP_QC_GETSURFACE)
	{"getsurfacenormal",PF_getsurfacenormal,436,	"vector(entity e, float s)"},// (DP_QC_GETSURFACE)
	{"getsurfacetexture",PF_getsurfacetexture,437,	"string(entity e, float s)"},// (DP_QC_GETSURFACE)
	{"getsurfacenearpoint",PF_getsurfacenearpoint,438,	"float(entity e, vector p)"},// (DP_QC_GETSURFACE)
	{"getsurfaceclippedpoint",PF_getsurfaceclippedpoint,439,	"vector(entity e, float s, vector p)"},// (DP_QC_GETSURFACE)
	{"clientcommand",	PF_clientcommand,	440,	"void(entity e, string s)"},// (KRIMZON_SV_PARSECLIENTCOMMAND)
	{"tokenize",		PF_Tokenize,		441,	"float(string s)"},// (KRIMZON_SV_PARSECLIENTCOMMAND)
	{"argv",			PF_ArgV,			442,	"string(float n)"},// (KRIMZON_SV_PARSECLIENTCOMMAND
	{"argc",			PF_ArgC,			0,		"float()"},
	{"setattachment",	PF_setattachment,	443,	"void(entity e, entity tagentity, string tagname)", ""},// (DP_GFX_QUAKE3MODELTAGS)
		{"search_begin",	PF_search_begin,			444,	"searchhandle(string pattern, optional float caseinsensitive, optional float quiet)", "stub. initiate a filesystem scan based upon filenames. Be sure to call search_end on the returned handle."},
		{"search_end",		PF_search_end,				445,	"void(searchhandle handle)", "stub."},
		{"search_getsize",	PF_search_getsize,			446,	"float(searchhandle handle)", "stub. Retrieves the number of files that were found."},
		{"search_getfilename", PF_search_getfilename,	447,	"string(searchhandle handle, float num)", "stub. Retrieves name of one of the files that was found by the initial search."},
		{"search_getfilesize", PF_search_getfilesize,	0,		"float(searchhandle handle, float num)", "stub. Retrieves the size of one of the files that was found by the initial search."},
		{"search_getfilemtime", PF_search_getfilemtime,	0,		"string(searchhandle handle, float num)", "stub. Retrieves modification time of one of the files in %Y-%m-%d %H:%M:%S format."},
	{"cvar_string",		PF_cvar_string,		448,	"string(string cvarname)"},//DP_QC_CVAR_STRING
	{"findflags",		PF_sv_findflags,	449,	"entity(entity start, .float fld, float match)"},//DP_QC_FINDFLAGS
	{"findchainflags",	PF_sv_findchainflags,450,	"entity(.float fld, float match)"},//DP_QC_FINDCHAINFLAGS
	{"dropclient",		PF_dropclient,		453,	"void(entity player)"},//DP_SV_BOTCLIENT
	{"spawnclient",		PF_spawnclient,		454,	"entity()", "Spawns a dummy player entity.\nNote that such dummy players will be carried from one map to the next.\nWarning: DP_SV_CLIENTCOLORS DP_SV_CLIENTNAME are not implemented in quakespasm, so use KRIMZON_SV_PARSECLIENTCOMMAND's clientcommand builtin to change the bot's name/colours/skin/team/etc, in the same way that clients would ask."},//DP_SV_BOTCLIENT
	{"clienttype",		PF_clienttype,		455,	"float(entity client)"},//botclient
	{"WriteUnterminatedString",PF_WriteString2,456,	"void(float target, string str)"},	//writestring but without the null terminator. makes things a little nicer.
	{"edict_num",		PF_edict_for_num,	459,	"entity(float entnum)"},//DP_QC_EDICT_NUM
	{"buf_create",		PF_buf_create,		460,	"strbuf()"},//DP_QC_STRINGBUFFERS
	{"buf_del",			PF_buf_del,			461,	"void(strbuf bufhandle)"},//DP_QC_STRINGBUFFERS
	{"buf_getsize",		PF_buf_getsize,		462,	"float(strbuf bufhandle)"},//DP_QC_STRINGBUFFERS
	{"buf_copy",		PF_buf_copy,		463,	"void(strbuf bufhandle_from, strbuf bufhandle_to)"},//DP_QC_STRINGBUFFERS
	{"buf_sort",		PF_buf_sort,		464,	"void(strbuf bufhandle, float sortprefixlen, float backward)"},//DP_QC_STRINGBUFFERS
	{"buf_implode",		PF_buf_implode,		465,	"string(strbuf bufhandle, string glue)"},//DP_QC_STRINGBUFFERS
	{"bufstr_get",		PF_bufstr_get,		466,	"string(strbuf bufhandle, float string_index)"},//DP_QC_STRINGBUFFERS
	{"bufstr_set",		PF_bufstr_set,		467,	"void(strbuf bufhandle, float string_index, string str)"},//DP_QC_STRINGBUFFERS
	{"bufstr_add",		PF_bufstr_add,		468,	"float(strbuf bufhandle, string str, float order)"},//DP_QC_STRINGBUFFERS
	{"bufstr_free",		PF_bufstr_free,		469,	"void(strbuf bufhandle, float string_index)"},//DP_QC_STRINGBUFFERS






	{"asin",			PF_asin,			471,	"float(float s)"},//DP_QC_ASINACOSATANATAN2TAN
	{"acos",			PF_acos,			472,	"float(float c)"},//DP_QC_ASINACOSATANATAN2TAN
	{"atan",			PF_atan,			473,	"float(float t)"},//DP_QC_ASINACOSATANATAN2TAN
	{"atan2",			PF_atan2,			474,	"float(float c, float s)"},//DP_QC_ASINACOSATANATAN2TAN
	{"tan",				PF_tan,				475,	"float(float a)"},//DP_QC_ASINACOSATANATAN2TAN
		{"strlennocol",		PF_strlennocol,		476,	D("float(string s)", "stub. Returns the number of characters in the string after any colour codes or other markup has been parsed.")},//DP_QC_STRINGCOLORFUNCTIONS
		{"strdecolorize",	PF_strdecolorize,	477,	D("string(string s)", "stub. Flattens any markup/colours, removing them from the string.")},//DP_QC_STRINGCOLORFUNCTIONS
	{"strftime",		PF_strftime,		478,	"string(float uselocaltime, string format, ...)"},	//DP_QC_STRFTIME
	{"tokenizebyseparator",PF_tokenizebyseparator,479,	"float(string s, string separator1, ...)"},	//DP_QC_TOKENIZEBYSEPARATOR
	{"strtolower",		PF_strtolower,		480,	"string(string s)"},	//DP_QC_STRING_CASE_FUNCTIONS
	{"strtoupper",		PF_strtoupper,		481,	"string(string s)"},	//DP_QC_STRING_CASE_FUNCTIONS
	{"cvar_defstring",	PF_cvar_defstring,	482,	"string(string s)"},	//DP_QC_CVAR_DEFSTRING
	{"pointsound",		PF_sv_pointsound,	483,	"void(vector origin, string sample, float volume, float attenuation)"},//DP_SV_POINTSOUND
	{"strreplace",		PF_strreplace,		484,	"string(string search, string replace, string subject)"},//DP_QC_STRREPLACE
	{"strireplace",		PF_strireplace,		485,	"string(string search, string replace, string subject)"},//DP_QC_STRREPLACE
	{"getsurfacepointattribute",PF_getsurfacepointattribute,486,	"vector(entity e, float s, float n, float a)"},//DP_QC_GETSURFACEPOINTATTRIBUTE

	{"crc16",			PF_crc16,			494,	"float(float caseinsensitive, string s, ...)"},//DP_QC_CRC16
	{"cvar_type",		PF_cvar_type,		495,	"float(string name)"},//DP_QC_CVAR_TYPE
	{"numentityfields",	PF_numentityfields,	496,	D("float()", "Gives the number of named entity fields. Note that this is not the size of an entity, but rather just the number of unique names (ie: vectors use 4 names rather than 3).")},//DP_QC_ENTITYDATA
	{"findentityfield",	PF_findentityfield,	0,		D("float(string fieldname)", "Find a field index by name.")},
	{"entityfieldref",	PF_entityfieldref,	0,		D("typedef .__variant field_t;\nfield_t(float fieldnum)", "Returns a field value that can be directly used to read entity fields. Be sure to validate the type with entityfieldtype before using.")},//DP_QC_ENTITYDATA
	{"entityfieldname",	PF_entityfieldname,	497,	D("string(float fieldnum)", "Retrieves the name of the given entity field.")},//DP_QC_ENTITYDATA
	{"entityfieldtype",	PF_entityfieldtype,	498,	D("float(float fieldnum)", "Provides information about the type of the field specified by the field num. Returns one of the EV_ values.")},//DP_QC_ENTITYDATA
	{"getentityfieldstring",PF_getentityfieldstring,499,	"string(float fieldnum, entity ent)"},//DP_QC_ENTITYDATA
	{"putentityfieldstring",PF_putentityfieldstring,500,	"float(float fieldnum, entity ent, string s)"},//DP_QC_ENTITYDATA
	{"whichpack",		PF_whichpack,		503,	D("string(string filename, optional float makereferenced)", "Returns the pak file name that contains the file specified. progs/player.mdl will generally return something like 'pak0.pak'. If makereferenced is true, clients will automatically be told that the returned package should be pre-downloaded and used, even if allow_download_refpackages is not set.")},//DP_QC_WHICHPACK
	{"uri_escape",		PF_uri_escape,		510,	"string(string in)"},//DP_QC_URI_ESCAPE
	{"uri_unescape",	PF_uri_unescape,	511,	"string(string in)"},//DP_QC_URI_ESCAPE
	{"num_for_edict",	PF_num_for_edict,	512,	"float(entity ent)"},//DP_QC_NUM_FOR_EDICT
	{"tokenize_console",PF_tokenize_console,514,	D("float(string str)", "Tokenize a string exactly as the console's tokenizer would do so. The regular tokenize builtin became bastardized for convienient string parsing, which resulted in a large disparity that can be exploited to bypass checks implemented in a naive SV_ParseClientCommand function, therefore you can use this builtin to make sure it exactly matches.")},
	{"argv_start_index",PF_argv_start_index,515,	D("float(float idx)", "Returns the character index that the tokenized arg started at.")},
	{"argv_end_index",	PF_argv_end_index,	516,	D("float(float idx)", "Returns the character index that the tokenized arg stopped at.")},
//	{"buf_cvarlist",	PF_buf_cvarlist,	517,	"void(strbuf strbuf, string pattern, string antipattern)"},
	{"cvar_description",PF_cvar_description,518,	D("string(string cvarname)", "Retrieves the description of a cvar, which might be useful for tooltips or help files. This may still not be useful.")},
	{"gettime",			PF_gettime,			519,	"float(optional float timetype)"},
//	{"loadfromdata",	PF_loadfromdata,	529,	D("void(string s)", "Reads a set of entities from the given string. This string should have the same format as a .ent file or a saved game. Entities will be spawned as required. If you need to see the entities that were created, you should use parseentitydata instead.")},
//	{"loadfromfile",	PF_loadfromfile,	530,	D("void(string s)", "Reads a set of entities from the named file. This file should have the same format as a .ent file or a saved game. Entities will be spawned as required. If you need to see the entities that were created, you should use parseentitydata instead.")},
	{"log",				PF_Logarithm,		532,	D("float(float v, optional float base)", "Determines the logarithm of the input value according to the specified base. This can be used to calculate how much something was shifted by.")},
	{"buf_loadfile",	PF_buf_loadfile,	535,	D("float(string filename, strbuf bufhandle)", "Appends the named file into a string buffer (which must have been created in advance). The return value merely says whether the file was readable.")},
	{"buf_writefile",	PF_buf_writefile,	536,	D("float(filestream filehandle, strbuf bufhandle, optional float startpos, optional float numstrings)", "Writes the contents of a string buffer onto the end of the supplied filehandle (you must have already used fopen). Additional optional arguments permit you to constrain the writes to a subsection of the stringbuffer.")},
	{"callfunction",	PF_callfunction,	605,	D("void(.../*, string funcname*/)", "Invokes the named function. The function name is always passed as the last parameter and must always be present. The others are passed to the named function as-is")},
	{"isfunction",		PF_isfunction,		607,	D("float(string s)", "Returns true if the named function exists and can be called with the callfunction builtin.")},
	{"parseentitydata",	PF_parseentitydata,	613,	D("float(entity e, string s, optional float offset)", "Reads a single entity's fields into an already-spawned entity. s should contain field pairs like in a saved game: {\"foo1\" \"bar\" \"foo2\" \"5\"}. Returns <=0 on failure, otherwise returns the offset in the string that was read to.")},
//	{"generateentitydata",PF_generateentitydata,0,	D("string(entity e)", "Dumps the entities fields into a string which can later be parsed with parseentitydata."}),
	{"sprintf",			PF_sprintf,			627,	"string(string fmt, ...)"},
	{"getsurfacenumtriangles",PF_getsurfacenumtriangles,628,"float(entity e, float s)"},
	{"getsurfacetriangle",PF_getsurfacetriangle,629,"vector(entity e, float s, float n)"},
//	{"digest_hex",		PF_digest_hex,		639,	"string(string digest, string data, ...)"},
};

static const char *extnames[] = 
{
	"DP_CON_SET",
	"DP_CON_SETA",
	"DP_EF_NOSHADOW",
	"DP_ENT_ALPHA",	//already in quakespasm, supposedly.
	"DP_ENT_COLORMOD",
	"DP_ENT_SCALE",
	"DP_ENT_TRAILEFFECTNUM",
	//"DP_GFX_QUAKE3MODELTAGS", //we support attachments but no md3/iqm/tags, so we can't really advertise this (although the builtin is complete if you ignore the lack of md3/iqms/tags)
	"DP_INPUTBUTTONS",
	"DP_QC_AUTOCVARS",	//they won't update on changes
	"DP_QC_ASINACOSATANATAN2TAN",
	"DP_QC_COPYENTITY",
	"DP_QC_CRC16",
	//"DP_QC_DIGEST",
	"DP_QC_CVAR_DEFSTRING",
	"DP_QC_CVAR_STRING",
	"DP_QC_CVAR_TYPE",
	"DP_QC_EDICT_NUM",
	"DP_QC_ENTITYDATA",
	"DP_QC_ETOS",
	"DP_QC_FINDCHAIN",
	"DP_QC_FINDCHAINFLAGS",
	"DP_QC_FINDCHAINFLOAT",
	"DP_QC_FINDFLAGS",
	"DP_QC_FINDFLOAT",
	"DP_QC_GETLIGHT",
	"DP_QC_GETSURFACE",
	"DP_QC_GETSURFACETRIANGLE",
	"DP_QC_GETSURFACEPOINTATTRIBUTE",
	"DP_QC_MINMAXBOUND",
	"DP_QC_MULTIPLETEMPSTRINGS",
	"DP_QC_RANDOMVEC",
	"DP_QC_SINCOSSQRTPOW",
	"DP_QC_STRFTIME",
	"DP_QC_STRING_CASE_FUNCTIONS",
	"DP_QC_STRINGBUFFERS",
//	"DP_QC_STRINGCOLORFUNCTIONS",	//the functions are provided only as stubs. the client has absolutely no support.
	"DP_QC_STRREPLACE",
	"DP_QC_TOKENIZEBYSEPARATOR",
	"DP_QC_TRACEBOX",
	"DP_QC_TRACETOSS",
	"DP_QC_TRACE_MOVETYPES",
	"DP_QC_URI_ESCAPE",
	"DP_QC_VECTOANGLES_WITH_ROLL",
	"DP_QC_VECTORVECTORS",
	"DP_QC_WHICHPACK",
	"DP_VIEWZOOM",
	"DP_REGISTERCVAR",
	"DP_SV_BOTCLIENT",
	"DP_SV_DROPCLIENT",
//	"DP_SV_POINTPARTICLES",	//can't enable this, because certain mods then assume that we're DP and all the particles break.
	"DP_SV_POINTSOUND",
	"DP_SV_SETCOLOR",
	"DP_SV_SPAWNFUNC_PREFIX",
	"DP_SV_WRITEUNTERMINATEDSTRING",
//	"DP_TE_BLOOD",
#ifdef PSET_SCRIPT
	"DP_TE_PARTICLERAIN",
	"DP_TE_PARTICLESNOW",
#endif
	"DP_TE_STANDARDEFFECTBUILTINS",
	"EXT_BITSHIFT",
	"FRIK_FILE",				//lacks the file part, but does have the strings part.
#ifdef PSET_SCRIPT
	"FTE_PART_SCRIPT",
	"FTE_PART_NAMESPACES",
#ifdef PSET_SCRIPT_EFFECTINFO
	"FTE_PART_NAMESPACE_EFFECTINFO",
#endif
#endif
	"FTE_QC_CHECKCOMMAND",
	"FTE_QC_CROSSPRODUCT",
	"FTE_QC_INTCONV",
	"FTE_STRINGS",
#ifdef PSET_SCRIPT
	"FTE_SV_POINTPARTICLES",
#endif
	"KRIMZON_SV_PARSECLIENTCOMMAND",
	"ZQ_QC_STRINGS",

};

static builtin_t extbuiltins[1024];

static void PF_checkextension(void)
{
	const char *extname = G_STRING(OFS_PARM0);
	unsigned int i;
	for (i = 0; i < sizeof(extnames)/sizeof(extnames[0]); i++)
	{
		if (!strcmp(extname, extnames[i]))
		{
			if (!pr_checkextension.value)
				Con_Printf("Mod found extension %s\n", extname);
			G_FLOAT(OFS_RETURN) = true;
			return;
		}
	}
	if (!pr_checkextension.value)
		Con_DPrintf("Mod tried extension %s\n", extname);
	G_FLOAT(OFS_RETURN) = false;
}

static void PF_EnableExtensionBuiltins(void)
{
	if (pr_builtins != extbuiltins)
	{	//first time we're using an extension! woo, everything is new!...
		memcpy(extbuiltins, pr_builtins, sizeof(pr_builtins[0])*pr_numbuiltins);
		pr_builtins = extbuiltins;
		for (; (unsigned int)pr_numbuiltins < sizeof(extbuiltins)/sizeof(extbuiltins[0]); )
			extbuiltins[pr_numbuiltins++] = PF_Fixme;
	}
}

static void PF_builtinsupported(void)
{
	const char *biname = G_STRING(OFS_PARM0);
	unsigned int i;
	for (i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]); i++)
	{
		if (!strcmp(extensionbuiltins[i].name, biname))
		{
			G_FLOAT(OFS_RETURN) = extensionbuiltins[i].number;
			return;
		}
	}
	G_FLOAT(OFS_RETURN) = 0;
}


static void PF_checkbuiltin (void)
{
	func_t funcref = G_INT(OFS_PARM0);
	if ((unsigned int)funcref < (unsigned int)progs->numfunctions)
	{
		dfunction_t *fnc = &pr_functions[(unsigned int)funcref];
//		const char *funcname = PR_GetString(fnc->s_name);
		int binum = -fnc->first_statement;
		unsigned int i;

		//qc defines the function at least. nothing weird there...
		if (binum > 0 && binum < pr_numbuiltins)
		{
			if (pr_builtins[binum] == PF_Fixme)
			{
				G_FLOAT(OFS_RETURN) = false;	//the builtin with that number isn't defined.
				for (i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]); i++)
				{
					if (extensionbuiltins[i].number == binum)
					{	//but it will be defined if its actually executed.
						if (extensionbuiltins[i].desc && !strncmp(extensionbuiltins[i].desc, "stub.", 5))
							G_FLOAT(OFS_RETURN) = false;	//pretend it won't work if it probably won't be useful
						else
							G_FLOAT(OFS_RETURN) = true;
						break;
					}
				}
			}
			else
			{
				G_FLOAT(OFS_RETURN) = true;		//its defined, within the sane range, mapped, everything. all looks good.
				//we should probably go through the available builtins and validate that the qc's name matches what would be expected
				//this is really intended more for builtins defined as #0 though, in such cases, mismatched assumptions are impossible.
			}
		}
		else
			G_FLOAT(OFS_RETURN) = false;	//not a valid builtin (#0 builtins get remapped at load, even if the builtin is activated then)
	}
	else
	{	//not valid somehow.
		G_FLOAT(OFS_RETURN) = false;
	}
}

void PF_Fixme (void)
{
	//interrogate the vm to try to figure out exactly which builtin they just tried to execute.
	dstatement_t *st = &pr_statements[pr_xstatement];
	eval_t *glob = (eval_t*)&pr_globals[st->a];
	if ((unsigned int)glob->function < (unsigned int)progs->numfunctions)
	{
		dfunction_t *fnc = &pr_functions[(unsigned int)glob->function];
		const char *funcname = PR_GetString(fnc->s_name);
		int binum = -fnc->first_statement;
		unsigned int i;
		if (binum >= 0)
		{
			//find an extension with the matching number
			for (i = 0; i < sizeof(extensionbuiltins) / sizeof(extensionbuiltins[0]); i++)
			{
				if (extensionbuiltins[i].number == binum)
				{	//set it up so we're faster next time
					PF_EnableExtensionBuiltins();
					if (!pr_checkextension.value || (extensionbuiltins[i].desc && !strncmp(extensionbuiltins[i].desc, "stub.", 5)))
						Con_Warning("Mod is using builtin #%u - %s\n", extensionbuiltins[i].documentednumber, extensionbuiltins[i].name);
					else
						Con_DPrintf2("Mod uses builtin #%u - %s\n", extensionbuiltins[i].documentednumber, extensionbuiltins[i].name);
					extbuiltins[binum] = extensionbuiltins[i].func;
					extensionbuiltins[i].func();
					return;
				}
			}

			PR_RunError ("unimplemented builtin #%i - %s", binum, funcname);
		}
	}
	PR_RunError ("PF_Fixme: not a builtin...");
}


//called at map end
void PR_ShutdownExtensions(void)
{
	PR_UnzoneAll();
	PF_buf_shutdown();
	tokenize_flush();
}

static func_t PR_FindExtFunction(const char *entryname)
{	//depends on 0 being an invalid function,
	dfunction_t *func = ED_FindFunction(entryname);
	if (func)
		return func - pr_functions;
	return 0;
}

void PR_AutoCvarChanged(cvar_t *var)
{
	char *n;
	ddef_t *glob;

	if (!sv.active)
		return;	//someone flushed our globals!..

	n = va("autocvar_%s", var->name);
	glob = ED_FindGlobal(n);
	if (glob)
	{
		if (!ED_ParseEpair ((void *)pr_globals, glob, var->string))
			Con_Warning("EXT: Unable to configure %s\n", n);
	}
}

//called at map start
void PR_EnableExtensions(ddef_t *pr_globaldefs)
{
	unsigned int i, j;
	unsigned int numautocvars = 0;

	static builtin_t *stdbuiltins;
	static int stdnumbuiltins;
	if (!stdbuiltins)
	{
		stdbuiltins = pr_builtins;
		stdnumbuiltins = pr_numbuiltins;

		//this also only needs to be done once. because we're evil. 
		//it should help slightly with the 'documentation' above at least.
		j = sizeof(extbuiltins)/sizeof(extbuiltins[0]);
		for (i = 1; i < sizeof(extensionbuiltins)/sizeof(extensionbuiltins[0]); i++)
		{
			if (extensionbuiltins[i].documentednumber)
				extensionbuiltins[i].number = extensionbuiltins[i].documentednumber;
			else
				extensionbuiltins[i].number = --j;
		}
	}
	pr_builtins = stdbuiltins;
	pr_numbuiltins = stdnumbuiltins;

	memset(&pr_extfuncs, 0, sizeof(pr_extfuncs));

	PR_ShutdownExtensions();	//just in case.

	if (!pr_checkextension.value)
	{
		Con_DPrintf("not enabling qc extensions\n");
		return;
	}

	PF_EnableExtensionBuiltins();
	extbuiltins[51] = PF_ext_vectoangles;

	pr_extfuncs.parseclientcommand = PR_FindExtFunction("SV_ParseClientCommand");
	pr_extfuncs.endframe = PR_FindExtFunction("EndFrame");

	//any #0 functions are remapped to their builtins here, so we don't have to tweak the VM in an obscure potentially-breaking way.
	for (i = 0; i < (unsigned int)progs->numfunctions; i++)
	{
		if (pr_functions[i].first_statement == 0 && pr_functions[i].s_name && !pr_functions[i].parm_start && !pr_functions[i].locals)
		{
			const char *name = PR_GetString(pr_functions[i].s_name);
			for (j = 0; j < sizeof(extensionbuiltins)/sizeof(extensionbuiltins[0]); j++)
			{
				if (!strcmp(extensionbuiltins[j].name, name))
				{	//okay, map it
					pr_functions[i].first_statement = -extensionbuiltins[j].number;
					break;
				}
			}
		}
	}

	//autocvars
	for (i = 0; i < (unsigned int)progs->numglobaldefs; i++)
	{
		const char *n = PR_GetString(pr_globaldefs[i].s_name);
		if (!strncmp(n, "autocvar_", 9))
		{
			//really crappy approach
			cvar_t *var = Cvar_Create(n + 9, PR_UglyValueString (pr_globaldefs[i].type, (eval_t*)(pr_globals + pr_globaldefs[i].ofs)));
			numautocvars++;
			if (!var)
				continue;	//name conflicts with a command?
	
			if (!ED_ParseEpair ((void *)pr_globals, &pr_globaldefs[i], var->string))
				Con_Warning("EXT: Unable to configure %s\n", n);
			var->flags |= CVAR_AUTOCVAR;
		}
	}
	if (numautocvars)
		Con_DPrintf2("Found %i autocvars\n", numautocvars);
}

void PR_DumpPlatform_f(void)
{
	char	name[MAX_OSPATH];
	FILE *f;
	const char *outname = "qsextensions";
	unsigned int i, j;
	for (i = 1; i < (unsigned)Cmd_Argc(); )
	{
		const char *arg = Cmd_Argv(i++);
		if (!strcmp(arg, "-O"))
		{
			if (arg[2])
				outname = arg+2;
			else
				outname = Cmd_Argv(i++);
		}
		else
		{
			Con_Printf("%s: Unknown argument\n", Cmd_Argv(0));
			return;
		}
	}

	if (strstr(outname, ".."))
		return;
	q_snprintf (name, sizeof(name), "%s/src/%s", com_gamedir, outname);
	COM_AddExtension (name, ".qc", sizeof(name));

	f = fopen (name, "w");
	if (!f)
	{
		Con_Printf("%s: Couldn't write %s\n", Cmd_Argv(0), name);
		return;
	}
	fprintf(f,
		"/*\n"
		"Extensions file for QuakeSpasm %1.2f.%d"BUILD_SPECIAL_STR"\n"
		"This file is auto-generated by %s %s.\n"
		"You will probably need to use FTEQCC to compile this.\n"
		"*/\n"
		,QUAKESPASM_VERSION, QUAKESPASM_VER_PATCH
		,Cmd_Argv(0), Cmd_Args()?Cmd_Args():"with no args");

	fprintf(f, 
		"\n\n//QuakeSpasm only supports ssqc, so including this file in some other situation is a user error\n"
		"#if defined(QUAKEWORLD) || defined(CSQC) || defined(MENU)\n"
		"#error Mixed up module defs\n"
		"#endif\n"
		);

	fprintf(f, "\n\n//List of advertised extensions\n");
	for (i = 0; i < sizeof(extnames)/sizeof(extnames[0]); i++)
		fprintf(f, "//%s\n", extnames[i]);

	fprintf(f, "\n\n//Explicitly flag this stuff as probably-not-referenced, meaning fteqcc will shut up about it and silently strip what it can.\n");
	fprintf(f, "#pragma noref 1\n");

	fprintf(f, "\n\n//Some custom types (that might be redefined as accessors by fteextensions.qc, although we don't define any methods here)\n");
	fprintf(f, "#ifdef _ACCESSORS\n");
	fprintf(f, "accessor strbuf:float;\n");
	fprintf(f, "accessor searchhandle:float;\n");
	fprintf(f, "accessor hashtable:float;\n");
	fprintf(f, "accessor infostring:string;\n");
	fprintf(f, "accessor filestream:float;\n");
	fprintf(f, "#else\n");
	fprintf(f, "#define strbuf float\n");
	fprintf(f, "#define searchhandle float\n");
	fprintf(f, "#define hashtable float\n");
	fprintf(f, "#define infostring string\n");
	fprintf(f, "#define filestream float\n");
	fprintf(f, "#endif\n");

	//extra fields
	fprintf(f, "\n\n//Supported Extension fields\n");
	fprintf(f, ".float gravity;\n");	//used by hipnotic
	fprintf(f, "//.float items2;			/*if defined, overrides serverflags for displaying runes on the hud*/\n");	//used by both mission packs. *REPLACES* serverflags if defined, so lets try not to define it.
	fprintf(f, ".float traileffectnum;		/*can also be set with 'traileffect' from a map editor*/\n");
	fprintf(f, ".float emiteffectnum;		/*can also be set with 'traileffect' from a map editor*/\n");
	fprintf(f, ".vector movement;			/*describes which forward/right/up keys the player is holidng*/\n");
	fprintf(f, ".entity viewmodelforclient;	/*attaches this entity to the specified player's view. invisible to other players*/\n");
	fprintf(f, ".float scale;				/*rescales the etntiy*/\n");
	fprintf(f, ".float alpha;				/*entity opacity*/\n");		//entity alpha. woot.
	fprintf(f, ".vector colormod;			/*tints the entity's colours*/\n");
	fprintf(f, ".entity tag_entity;\n");
	fprintf(f, ".float tag_index;\n");
	fprintf(f, ".float button3;\n");
	fprintf(f, ".float button4;\n");
	fprintf(f, ".float button5;\n");
	fprintf(f, ".float button6;\n");
	fprintf(f, ".float button7;\n");
	fprintf(f, ".float button8;\n");
	fprintf(f, ".float viewzoom;			/*rescales the user's fov*/\n");
	fprintf(f, ".float modelflags;			/*provides additional modelflags to use (effects&EF_NOMODELFLAGS to replace the original model's)*/\n");

	//extra constants
	fprintf(f, "\n\n//Supported Extension Constants\n");
	fprintf(f, "const float MOVETYPE_FOLLOW	= "STRINGIFY(MOVETYPE_EXT_FOLLOW)";\n");
	fprintf(f, "const float SOLID_CORPSE	= "STRINGIFY(SOLID_EXT_CORPSE)";\n");

	fprintf(f, "const float FILE_READ		= "STRINGIFY(0)";\n");
	fprintf(f, "const float FILE_APPEND		= "STRINGIFY(1)";\n");
	fprintf(f, "const float FILE_WRITE		= "STRINGIFY(2)";\n");

	fprintf(f, "const float CLIENTTYPE_DISCONNECT	= "STRINGIFY(0)";\n");
	fprintf(f, "const float CLIENTTYPE_REAL			= "STRINGIFY(1)";\n");
	fprintf(f, "const float CLIENTTYPE_BOT			= "STRINGIFY(2)";\n");
	fprintf(f, "const float CLIENTTYPE_NOTCLIENT	= "STRINGIFY(3)";\n");

	fprintf(f, "const float EF_NOSHADOW			= %#x;\n", EF_NOSHADOW);
	fprintf(f, "const float EF_NOMODELFLAGS		= %#x; /*the standard modelflags from the model are ignored*/\n", EF_NOMODELFLAGS);

	fprintf(f, "const float MF_ROCKET			= %#x;\n", EF_ROCKET);
	fprintf(f, "const float MF_GRENADE			= %#x;\n", EF_GRENADE);
	fprintf(f, "const float MF_GIB				= %#x;\n", EF_GIB);
	fprintf(f, "const float MF_ROTATE			= %#x;\n", EF_ROTATE);
	fprintf(f, "const float MF_TRACER			= %#x;\n", EF_TRACER);
	fprintf(f, "const float MF_ZOMGIB			= %#x;\n", EF_ZOMGIB);
	fprintf(f, "const float MF_TRACER2			= %#x;\n", EF_TRACER2);
	fprintf(f, "const float MF_TRACER3			= %#x;\n", EF_TRACER3);

	fprintf(f, "const float MSG_MULTICAST	= "STRINGIFY(4)";\n");
	fprintf(f, "const float MULTICAST_ALL	= "STRINGIFY(MULTICAST_ALL_U)";\n");
//	fprintf(f, "const float MULTICAST_PHS	= "STRINGIFY(MULTICAST_PHS_U)";\n");
	fprintf(f, "const float MULTICAST_PVS	= "STRINGIFY(MULTICAST_PVS_U)";\n");
	fprintf(f, "const float MULTICAST_ONE	= "STRINGIFY(MULTICAST_ONE_U)";\n");
	fprintf(f, "const float MULTICAST_ALL_R	= "STRINGIFY(MULTICAST_ALL_R)";\n");
//	fprintf(f, "const float MULTICAST_PHS_R	= "STRINGIFY(MULTICAST_PHS_R)";\n");
	fprintf(f, "const float MULTICAST_PVS_R	= "STRINGIFY(MULTICAST_PVS_R)";\n");
	fprintf(f, "const float MULTICAST_ONE_R	= "STRINGIFY(MULTICAST_ONE_R)";\n");
	fprintf(f, "const float MULTICAST_INIT	= "STRINGIFY(MULTICAST_INIT)";\n");

	for (j = 0; j < 2; j++)
	{
		if (j)
			fprintf(f, "\n\n//Builtin Stubs List (these are present for simpler compatibility, but not properly supported in QuakeSpasm at this time).\n/*\n");
		else
			fprintf(f, "\n\n//Builtin list\n");
		for (i = 0; i < sizeof(extensionbuiltins)/sizeof(extensionbuiltins[0]); i++)
		{
			if (j != (extensionbuiltins[i].desc?!strncmp(extensionbuiltins[i].desc, "stub.", 5):0))
				continue;
			fprintf(f, "%s %s = #%i;", extensionbuiltins[i].typestr, extensionbuiltins[i].name, extensionbuiltins[i].documentednumber);
			if (extensionbuiltins[i].desc && !j)
			{
				const char *line = extensionbuiltins[i].desc;
				const char *term;
				fprintf(f, " /*");
				while(*line)
				{
					fprintf(f, "\n\t\t");
					term = line;
					while(*term && *term != '\n')
						term++;
					fwrite(line, 1, term - line, f);
					if (*term == '\n')
					{
						term++;
					}
					line = term;
				}
				fprintf(f, " */\n\n");
			}
			else
				fprintf(f, "\n");
		}
		if (j)
			fprintf(f, "*/\n");
	}

	fprintf(f, "\n\n//Reset this back to normal.\n");
	fprintf(f, "#pragma noref 0\n");
	fclose(f);
}

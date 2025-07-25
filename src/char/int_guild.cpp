// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "int_guild.hpp"

#include <stdlib.h>
#define __STDC_WANT_LIB_EXT1__ 1
#include <string.h>

#include "../common/cbasetypes.hpp"
#include "../common/malloc.hpp"
#include "../common/mmo.hpp"
#include "../common/showmsg.hpp"
#include "../common/socket.hpp"
#include "../common/strlib.hpp"
#include "../common/timer.hpp"

#include "char.hpp"
#include "char_mapif.hpp"
#include "inter.hpp"

#define GS_MEMBER_UNMODIFIED 0x00
#define GS_MEMBER_MODIFIED 0x01
#define GS_MEMBER_NEW 0x02

#define GS_POSITION_UNMODIFIED 0x00
#define GS_POSITION_MODIFIED 0x01

// LSB = 0 => Alliance, LSB = 1 => Opposition
#define GUILD_ALLIANCE_TYPE_MASK 0x01
#define GUILD_ALLIANCE_REMOVE 0x08

static const char dataToHex[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

//Guild cache
static DBMap* guild_db_; // int guild_id -> struct guild*
static DBMap *castle_db;

static unsigned int guild_exp[100];

int mapif_parse_GuildLeave(int fd,int guild_id,uint32 account_id,uint32 char_id,int flag,const char *mes);
int mapif_guild_broken(int guild_id,int flag);
bool guild_check_empty(struct guild *g);
int guild_calcinfo(struct guild *g);
int mapif_guild_basicinfochanged(int guild_id,int type,const void *data,int len);
int mapif_guild_info(int fd,struct guild *g);
int guild_break_sub(int key,void *data,va_list ap);
int inter_guild_tosql(struct guild *g,int flag);
int guild_checkskill(struct guild *g, int id);

TIMER_FUNC(guild_save_timer){
	static int last_id = 0; //To know in which guild we were.
	int state = 0; //0: Have not reached last guild. 1: Reached last guild, ready for save. 2: Some guild saved, don't do further saving.
	DBIterator *iter = db_iterator(guild_db_);
	DBKey key;
	struct guild* g;

	if( last_id == 0 ) //Save the first guild in the list.
		state = 1;

	for( g = (struct guild*)db_data2ptr(iter->first(iter, &key)); dbi_exists(iter); g = (struct guild*)db_data2ptr(iter->next(iter, &key)) )
	{
		if( state == 0 && g->guild_id == last_id )
			state++; //Save next guild in the list.
		else if( state == 1 && g->save_flag&GS_MASK )
		{
			inter_guild_tosql(g, g->save_flag&GS_MASK);
			g->save_flag &= ~GS_MASK;

			//Some guild saved.
			last_id = g->guild_id;
			state++;
		}

		if( g->save_flag == GS_REMOVE )
		{// Nothing to save, guild is ready for removal.
			if (charserv_config.save_log)
				ShowInfo("Guild Unloaded (%d - %s)\n", g->guild_id, g->name);
			db_remove(guild_db_, key);
		}
	}
	dbi_destroy(iter);

	if( state != 2 ) //Reached the end of the guild db without saving.
		last_id = 0; //Reset guild saved, return to beginning.

	state = guild_db_->size(guild_db_);
	if( state < 1 ) state = 1; //Calculate the time slot for the next save.
	add_timer(tick  + (charserv_config.autosave_interval)/state, guild_save_timer, 0, 0);
	return 0;
}

int inter_guild_removemember_tosql(uint32 char_id)
{
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE from `%s` where `char_id` = '%d'", schema_config.guild_member_db, char_id) )
		Sql_ShowDebug(sql_handle);
	if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `guild_id` = '0' WHERE `char_id` = '%d'", schema_config.char_db, char_id) )
		Sql_ShowDebug(sql_handle);
	return 0;
}

// Save guild into sql
int inter_guild_tosql(struct guild *g,int flag)
{
	// Table guild (GS_BASIC_MASK)
	// GS_EMBLEM `emblem_len`,`emblem_id`,`emblem_data`
	// GS_CONNECT `connect_member`,`average_lv`
	// GS_MES `mes1`,`mes2`
	// GS_LEVEL `guild_lv`,`max_member`,`exp`,`next_exp`,`skill_point`
	// GS_BASIC `name`,`master`,`char_id`

	// GS_MEMBER `guild_member` (`guild_id`,`char_id`,`exp`,`position`)
	// GS_POSITION `guild_position` (`guild_id`,`position`,`name`,`mode`,`exp_mode`)
	// GS_ALLIANCE `guild_alliance` (`guild_id`,`opposition`,`alliance_id`,`name`)
	// GS_EXPULSION `guild_expulsion` (`guild_id`,`account_id`,`name`,`mes`)
	// GS_SKILL `guild_skill` (`guild_id`,`id`,`lv`)

	// temporary storage for str convertion. They must be twice the size of the
	// original string to ensure no overflows will occur. [Skotlex]
	char t_info[256];
	char esc_name[NAME_LENGTH*2+1];
	char esc_master[NAME_LENGTH*2+1];
	char new_guild = 0;
	int i=0;

	if (g->guild_id<=0 && g->guild_id != -1) return 0;

#ifdef NOISY
	ShowInfo("Save guild request (" CL_BOLD "%d" CL_RESET " - flag 0x%x).",g->guild_id, flag);
#endif

	Sql_EscapeStringLen(sql_handle, esc_name, g->name, strnlen(g->name, NAME_LENGTH));
	Sql_EscapeStringLen(sql_handle, esc_master, g->master, strnlen(g->master, NAME_LENGTH));
	*t_info = '\0';

	// Insert a new guild the guild
	if (flag&GS_BASIC && g->guild_id == -1)
	{
		strcat(t_info, " guild_create");

		// Create a new guild
		if( SQL_ERROR == Sql_Query(sql_handle, "INSERT INTO `%s` "
			"(`name`,`master`,`guild_lv`,`max_member`,`average_lv`,`char_id`) "
			"VALUES ('%s', '%s', '%d', '%d', '%d', '%d')",
			schema_config.guild_db, esc_name, esc_master, g->guild_lv, g->max_member, g->average_lv, g->member[0].char_id) )
		{
			Sql_ShowDebug(sql_handle);
			return 0; //Failed to create guild!
		}
		else
		{
			g->guild_id = (int)Sql_LastInsertId(sql_handle);
			new_guild = 1;
		}
	}

	// If we need an update on an existing guild or more update on the new guild
	if (((flag & GS_BASIC_MASK) && !new_guild) || ((flag & (GS_BASIC_MASK & ~GS_BASIC)) && new_guild))
	{
		StringBuf buf;
		bool add_comma = false;

		StringBuf_Init(&buf);
		StringBuf_Printf(&buf, "UPDATE `%s` SET ", schema_config.guild_db);

		if (flag & GS_EMBLEM)
		{
			char emblem_data[sizeof(g->emblem_data)*2+1];
			char* pData = emblem_data;

			strcat(t_info, " emblem");
			// Convert emblem_data to hex
			//TODO: why not use binary directly? [ultramage]
			for(i=0; i<g->emblem_len; i++){
				*pData++ = dataToHex[(g->emblem_data[i] >> 4) & 0x0F];
				*pData++ = dataToHex[g->emblem_data[i] & 0x0F];
			}
			*pData = 0;
			StringBuf_Printf(&buf, "`emblem_len`=%d, `emblem_id`=%d, `emblem_data`='%s'", g->emblem_len, g->emblem_id, emblem_data);
			add_comma = true;
		}
		if (flag & GS_BASIC)
		{
			strcat(t_info, " basic");
			if( add_comma )
				StringBuf_AppendStr(&buf, ", ");
			else
				add_comma = true;
			StringBuf_Printf(&buf, "`name`='%s', `master`='%s', `char_id`=%d", esc_name, esc_master, g->member[0].char_id);

			if (g->last_leader_change)
				StringBuf_Printf(&buf, ", `last_master_change`=FROM_UNIXTIME(%d)", g->last_leader_change);
		}
		if (flag & GS_CONNECT)
		{
			strcat(t_info, " connect");
			if( add_comma )
				StringBuf_AppendStr(&buf, ", ");
			else
				add_comma = true;
			StringBuf_Printf(&buf, "`connect_member`=%d, `average_lv`=%d", g->connect_member, g->average_lv);
		}
		if (flag & GS_MES)
		{
			char esc_mes1[sizeof(g->mes1)*2+1];
			char esc_mes2[sizeof(g->mes2)*2+1];

			strcat(t_info, " mes");
			if( add_comma )
				StringBuf_AppendStr(&buf, ", ");
			else
				add_comma = true;
			Sql_EscapeStringLen(sql_handle, esc_mes1, g->mes1, strnlen(g->mes1, sizeof(g->mes1)));
			Sql_EscapeStringLen(sql_handle, esc_mes2, g->mes2, strnlen(g->mes2, sizeof(g->mes2)));
			StringBuf_Printf(&buf, "`mes1`='%s', `mes2`='%s'", esc_mes1, esc_mes2);
		}
		if (flag & GS_LEVEL)
		{
			strcat(t_info, " level");
			if( add_comma )
				StringBuf_AppendStr(&buf, ", ");
			//else	//last condition using add_coma setting
			//	add_comma = true;
			StringBuf_Printf(&buf, "`guild_lv`=%d, `skill_point`=%d, `exp`=%" PRIu64 ", `next_exp`=%u, `max_member`=%d", g->guild_lv, g->skill_point, g->exp, g->next_exp, g->max_member);
		}
		StringBuf_Printf(&buf, " WHERE `guild_id`=%d", g->guild_id);
		if( SQL_ERROR == Sql_Query(sql_handle, "%s", StringBuf_Value(&buf)) )
			Sql_ShowDebug(sql_handle);
		StringBuf_Destroy(&buf);
	}

	if (flag&GS_MEMBER)
	{
		strcat(t_info, " members");
		// Update only needed players
		for(i=0;i<g->max_member;i++){
			struct guild_member *m = &g->member[i];
			if (!m->modified)
				continue;
			if(m->account_id) {
				//Since nothing references guild member table as foreign keys, it's safe to use REPLACE INTO
				if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` (`guild_id`,`char_id`,`exp`,`position`) "
					"VALUES ('%d','%d','%" PRIu64 "','%d')",
					schema_config.guild_member_db, g->guild_id, m->char_id, m->exp, m->position ) )
					Sql_ShowDebug(sql_handle);
				if (m->modified&GS_MEMBER_NEW || new_guild == 1)
				{
					if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `guild_id` = '%d' WHERE `char_id` = '%d'",
						schema_config.char_db, g->guild_id, m->char_id) )
						Sql_ShowDebug(sql_handle);
				}
				m->modified = GS_MEMBER_UNMODIFIED;
			}
		}
	}

	if (flag&GS_POSITION){
		strcat(t_info, " positions");
		//printf("- Insert guild %d to guild_position\n",g->guild_id);
		for(i=0;i<MAX_GUILDPOSITION;i++){
			struct guild_position *p = &g->position[i];
			if (!p->modified)
				continue;
			Sql_EscapeStringLen(sql_handle, esc_name, p->name, strnlen(p->name, NAME_LENGTH));
			if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` (`guild_id`,`position`,`name`,`mode`,`exp_mode`) VALUES ('%d','%d','%s','%d','%d')",
				schema_config.guild_position_db, g->guild_id, i, esc_name, p->mode, p->exp_mode) )
				Sql_ShowDebug(sql_handle);
			p->modified = GS_POSITION_UNMODIFIED;
		}
	}

	if (flag&GS_ALLIANCE)
	{
		// Delete current alliances
		// NOTE: no need to do it on both sides since both guilds in memory had
		// their info changed, not to mention this would also mess up oppositions!
		// [Skotlex]
		//if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id`='%d' OR `alliance_id`='%d'", guild_alliance_db, g->guild_id, g->guild_id) )
		if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id`='%d'", schema_config.guild_alliance_db, g->guild_id) )
		{
			Sql_ShowDebug(sql_handle);
		}
		else
		{
			//printf("- Insert guild %d to guild_alliance\n",g->guild_id);
			for(i=0;i<MAX_GUILDALLIANCE;i++)
			{
				struct guild_alliance *a=&g->alliance[i];
				if(a->guild_id>0)
				{
					Sql_EscapeStringLen(sql_handle, esc_name, a->name, strnlen(a->name, NAME_LENGTH));
					if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` (`guild_id`,`opposition`,`alliance_id`,`name`) "
						"VALUES ('%d','%d','%d','%s')",
						schema_config.guild_alliance_db, g->guild_id, a->opposition, a->guild_id, esc_name) )
						Sql_ShowDebug(sql_handle);
				}
			}
		}
	}

	if (flag&GS_EXPULSION){
		strcat(t_info, " expulsions");
		//printf("- Insert guild %d to guild_expulsion\n",g->guild_id);
		for(i=0;i<MAX_GUILDEXPULSION;i++){
			struct guild_expulsion *e=&g->expulsion[i];
			if(e->account_id>0){
				char esc_mes[sizeof(e->mes)*2+1];

				Sql_EscapeStringLen(sql_handle, esc_name, e->name, strnlen(e->name, NAME_LENGTH));
				Sql_EscapeStringLen(sql_handle, esc_mes, e->mes, strnlen(e->mes, sizeof(e->mes)));
				if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` (`guild_id`,`account_id`,`name`,`mes`) "
					"VALUES ('%d','%d','%s','%s')", schema_config.guild_expulsion_db, g->guild_id, e->account_id, esc_name, esc_mes) )
					Sql_ShowDebug(sql_handle);
			}
		}
	}

	if (flag&GS_SKILL){
		strcat(t_info, " skills");
		//printf("- Insert guild %d to guild_skill\n",g->guild_id);
		for(i=0;i<MAX_GUILDSKILL;i++){
			if (g->skill[i].id>0 && g->skill[i].lv>0){
				if( SQL_ERROR == Sql_Query(sql_handle, "REPLACE INTO `%s` (`guild_id`,`id`,`lv`) VALUES ('%d','%d','%d')",
					schema_config.guild_skill_db, g->guild_id, g->skill[i].id, g->skill[i].lv) )
					Sql_ShowDebug(sql_handle);
			}
		}
	}

	if (charserv_config.save_log)
		ShowInfo("Saved guild (%d - %s):%s\n",g->guild_id,g->name,t_info);
	return 1;
}

// Read guild from sql
struct guild * inter_guild_fromsql(int guild_id)
{
	struct guild *g;
	char* data;
	size_t len;
	char* p;
	int i;

	if( guild_id <= 0 )
		return NULL;

	g = (struct guild*)idb_get(guild_db_, guild_id);
	if( g )
		return g;

#ifdef NOISY
	ShowInfo("Guild load request (%d)...\n", guild_id);
#endif

	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT g.`name`,c.`name`,g.`guild_lv`,g.`connect_member`,g.`max_member`,g.`average_lv`,g.`exp`,g.`next_exp`,g.`skill_point`,g.`mes1`,g.`mes2`,g.`emblem_len`,g.`emblem_id`,COALESCE(UNIX_TIMESTAMP(g.`last_master_change`),0), g.`emblem_data` "
		"FROM `%s` g LEFT JOIN `%s` c ON c.`char_id` = g.`char_id` WHERE g.`guild_id`='%d'", schema_config.guild_db, schema_config.char_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		return NULL;
	}

	if( SQL_SUCCESS != Sql_NextRow(sql_handle) )
		return NULL;// Guild does not exists.

	CREATE(g, struct guild, 1);

	g->guild_id = guild_id;
	Sql_GetData(sql_handle,  0, &data, &len); memcpy(g->name, data, zmin(len, NAME_LENGTH));
	Sql_GetData(sql_handle,  1, &data, &len); memcpy(g->master, data, zmin(len, NAME_LENGTH));
	Sql_GetData(sql_handle,  2, &data, NULL); g->guild_lv = atoi(data);
	Sql_GetData(sql_handle,  3, &data, NULL); g->connect_member = atoi(data);
	Sql_GetData(sql_handle,  4, &data, NULL); g->max_member = atoi(data);
	if( g->max_member > MAX_GUILD )
	{	// Fix reduction of MAX_GUILD [PoW]
		ShowWarning("Guild %d:%s specifies higher capacity (%d) than MAX_GUILD (%d)\n", guild_id, g->name, g->max_member, MAX_GUILD);
		g->max_member = MAX_GUILD;
	}
	Sql_GetData(sql_handle,  5, &data, NULL); g->average_lv = atoi(data);
	Sql_GetData(sql_handle,  6, &data, NULL); g->exp = strtoull(data, NULL, 10);
	Sql_GetData(sql_handle,  7, &data, NULL); g->next_exp = (unsigned int)strtoul(data, NULL, 10);
	Sql_GetData(sql_handle,  8, &data, NULL); g->skill_point = atoi(data);
	Sql_GetData(sql_handle,  9, &data, &len); memcpy(g->mes1, data, zmin(len, sizeof(g->mes1)));
	Sql_GetData(sql_handle, 10, &data, &len); memcpy(g->mes2, data, zmin(len, sizeof(g->mes2)));
	Sql_GetData(sql_handle, 11, &data, &len); g->emblem_len = atoi(data);
	Sql_GetData(sql_handle, 12, &data, &len); g->emblem_id = atoi(data);
	Sql_GetData(sql_handle, 13, &data, NULL); g->last_leader_change = atoi(data);
	Sql_GetData(sql_handle, 14, &data, &len);
	// convert emblem data from hexadecimal to binary
	//TODO: why not store it in the db as binary directly? [ultramage]
	for( i = 0, p = g->emblem_data; i < g->emblem_len; ++i, ++p )
	{
		if( *data >= '0' && *data <= '9' )
			*p = *data - '0';
		else if( *data >= 'a' && *data <= 'f' )
			*p = *data - 'a' + 10;
		else if( *data >= 'A' && *data <= 'F' )
			*p = *data - 'A' + 10;
		*p <<= 4;
		++data;

		if( *data >= '0' && *data <= '9' )
			*p |= *data - '0';
		else if( *data >= 'a' && *data <= 'f' )
			*p |= *data - 'a' + 10;
		else if( *data >= 'A' && *data <= 'F' )
			*p |= *data - 'A' + 10;
		++data;
	}

	// load guild member info
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `c`.`account_id`,`m`.`char_id`,`c`.`hair`,`c`.`hair_color`,`c`.`sex`,`c`.`class`,`c`.`base_level`,`m`.`exp`,`c`.`online`,`m`.`position`,`c`.`name`,coalesce(UNIX_TIMESTAMP(`c`.`last_login`),0) "
		"FROM `%s` `m` INNER JOIN `%s` `c` on `c`.`char_id`=`m`.`char_id` WHERE `m`.`guild_id`='%d' ORDER BY `position`", schema_config.guild_member_db, schema_config.char_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		aFree(g);
		return NULL;
	}
	for( i = 0; i < g->max_member && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
	{
		struct guild_member* m = &g->member[i];

		Sql_GetData(sql_handle,  0, &data, NULL); m->account_id = atoi(data);
		Sql_GetData(sql_handle,  1, &data, NULL); m->char_id = atoi(data);
		Sql_GetData(sql_handle,  2, &data, NULL); m->hair = atoi(data);
		Sql_GetData(sql_handle,  3, &data, NULL); m->hair_color = atoi(data);
		Sql_GetData(sql_handle,  4, &data, NULL);
		switch( *data ){
			case 'F':
				m->gender = SEX_FEMALE;
				break;
			case 'M':
				m->gender = SEX_MALE;
				break;
			default:
				ShowWarning( "inter_guild_fromsql: Unsupported gender %c for char_id %u. Defaulting to male...\n", *data, m->char_id );
				m->gender = SEX_MALE;
				break;
		}
		Sql_GetData(sql_handle,  5, &data, NULL); m->class_ = atoi(data);
		Sql_GetData(sql_handle,  6, &data, NULL); m->lv = atoi(data);
		Sql_GetData(sql_handle,  7, &data, NULL); m->exp = strtoull(data, NULL, 10);
		Sql_GetData(sql_handle,  8, &data, NULL); m->online = atoi(data);
		Sql_GetData(sql_handle,  9, &data, NULL); m->position = atoi(data);
		if( m->position >= MAX_GUILDPOSITION ) // Fix reduction of MAX_GUILDPOSITION [PoW]
			m->position = MAX_GUILDPOSITION - 1;
		Sql_GetData(sql_handle, 10, &data, &len); memcpy(m->name, data, zmin(len, NAME_LENGTH));
		Sql_GetData(sql_handle, 11, &data, NULL); m->last_login = atoi(data);
		m->modified = GS_MEMBER_UNMODIFIED;
	}

	//printf("- Read guild_position %d from sql \n",guild_id);
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `position`,`name`,`mode`,`exp_mode` FROM `%s` WHERE `guild_id`='%d'", schema_config.guild_position_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		aFree(g);
		return NULL;
	}
	while( SQL_SUCCESS == Sql_NextRow(sql_handle) )
	{
		int position;
		struct guild_position* gpos;

		Sql_GetData(sql_handle, 0, &data, NULL); position = atoi(data);
		if( position < 0 || position >= MAX_GUILDPOSITION )
			continue;// invalid position
		gpos = &g->position[position];
		Sql_GetData(sql_handle, 1, &data, &len); memcpy(gpos->name, data, zmin(len, NAME_LENGTH));
		Sql_GetData(sql_handle, 2, &data, NULL); gpos->mode = atoi(data);
		Sql_GetData(sql_handle, 3, &data, NULL); gpos->exp_mode = atoi(data);
		gpos->modified = GS_POSITION_UNMODIFIED;
	}

	//printf("- Read guild_alliance %d from sql \n",guild_id);
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `opposition`,`alliance_id`,`name` FROM `%s` WHERE `guild_id`='%d'", schema_config.guild_alliance_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		aFree(g);
		return NULL;
	}
	for( i = 0; i < MAX_GUILDALLIANCE && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
	{
		struct guild_alliance* a = &g->alliance[i];

		Sql_GetData(sql_handle, 0, &data, NULL); a->opposition = atoi(data);
		Sql_GetData(sql_handle, 1, &data, NULL); a->guild_id = atoi(data);
		Sql_GetData(sql_handle, 2, &data, &len); memcpy(a->name, data, zmin(len, NAME_LENGTH));
	}

	//printf("- Read guild_expulsion %d from sql \n",guild_id);
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `account_id`,`name`,`mes` FROM `%s` WHERE `guild_id`='%d'", schema_config.guild_expulsion_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		aFree(g);
		return NULL;
	}
	for( i = 0; i < MAX_GUILDEXPULSION && SQL_SUCCESS == Sql_NextRow(sql_handle); ++i )
	{
		struct guild_expulsion *e = &g->expulsion[i];

		Sql_GetData(sql_handle, 0, &data, NULL); e->account_id = atoi(data);
		Sql_GetData(sql_handle, 1, &data, &len); memcpy(e->name, data, zmin(len, NAME_LENGTH));
		Sql_GetData(sql_handle, 2, &data, &len); memcpy(e->mes, data, zmin(len, sizeof(e->mes)));
	}

	//printf("- Read guild_skill %d from sql \n",guild_id);
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `id`,`lv` FROM `%s` WHERE `guild_id`='%d' ORDER BY `id`", schema_config.guild_skill_db, guild_id) )
	{
		Sql_ShowDebug(sql_handle);
		aFree(g);
		return NULL;
	}

	for(i = 0; i < MAX_GUILDSKILL; i++)
	{	//Skill IDs must always be initialized. [Skotlex]
		g->skill[i].id = i + GD_SKILLBASE;
	}

	while( SQL_SUCCESS == Sql_NextRow(sql_handle) )
	{
		int id;
		Sql_GetData(sql_handle, 0, &data, NULL); id = atoi(data) - GD_SKILLBASE;
		if( id < 0 || id >= MAX_GUILDSKILL )
			continue;// invalid guild skill
		Sql_GetData(sql_handle, 1, &data, NULL); g->skill[id].lv = atoi(data);
	}
	Sql_FreeResult(sql_handle);

	idb_put(guild_db_, guild_id, g); //Add to cache
	g->save_flag |= GS_REMOVE; //But set it to be removed, in case it is not needed for long.

	if (charserv_config.save_log)
		ShowInfo("Guild loaded (%d - %s)\n", guild_id, g->name);

	return g;
}

/**
 * Get the max storage size of a guild.
 * @param guild_id: Guild ID to search
 * @return Guild storage max size
 */
uint16 inter_guild_storagemax(int guild_id)
{
#ifdef OFFICIAL_GUILD_STORAGE
	struct guild *g = inter_guild_fromsql(guild_id);
	uint16 max = 0;

	if (!g) {
		ShowError("Guild %d not found!\n", guild_id);
		return 0;
	}

	max = guild_checkskill(g, GD_GUILD_STORAGE);
	if (max)
		max *= 100;

	return max;
#else
	return MAX_GUILD_STORAGE;
#endif
}

// `guild_castle` (`castle_id`, `guild_id`, `economy`, `defense`, `triggerE`, `triggerD`, `nextTime`, `payTime`, `createTime`, `visibleC`, `visibleG0`, `visibleG1`, `visibleG2`, `visibleG3`, `visibleG4`, `visibleG5`, `visibleG6`, `visibleG7`)
int inter_guildcastle_tosql(struct guild_castle *gc)
{
	StringBuf buf;
	int i;

	StringBuf_Init(&buf);
	StringBuf_Printf(&buf, "REPLACE INTO `%s` SET `castle_id`='%d', `guild_id`='%d', `economy`='%d', `defense`='%d', "
	    "`triggerE`='%d', `triggerD`='%d', `nextTime`='%d', `payTime`='%d', `createTime`='%d', `visibleC`='%d'",
	    schema_config.guild_castle_db, gc->castle_id, gc->guild_id, gc->economy, gc->defense,
	    gc->triggerE, gc->triggerD, gc->nextTime, gc->payTime, gc->createTime, gc->visibleC);
	for (i = 0; i < MAX_GUARDIANS; ++i)
		StringBuf_Printf(&buf, ", `visibleG%d`='%d'", i, gc->guardian[i].visible);

	if (SQL_ERROR == Sql_Query(sql_handle, StringBuf_Value(&buf)))
		Sql_ShowDebug(sql_handle);
	else if(charserv_config.save_log)
		ShowInfo("Saved guild castle (%d)\n", gc->castle_id);

	StringBuf_Destroy(&buf);
	return 0;
}

// Read guild_castle from SQL
struct guild_castle* inter_guildcastle_fromsql(int castle_id)
{
	char *data;
	int i;
	StringBuf buf;
	struct guild_castle *gc = (struct guild_castle *)idb_get(castle_db, castle_id);

	if (gc != NULL)
		return gc;

	StringBuf_Init(&buf);
	StringBuf_AppendStr(&buf, "SELECT `castle_id`, `guild_id`, `economy`, `defense`, `triggerE`, "
	                    "`triggerD`, `nextTime`, `payTime`, `createTime`, `visibleC`");
	for (i = 0; i < MAX_GUARDIANS; ++i)
		StringBuf_Printf(&buf, ", `visibleG%d`", i);
	StringBuf_Printf(&buf, " FROM `%s` WHERE `castle_id`='%d'", schema_config.guild_castle_db, castle_id);
	if (SQL_ERROR == Sql_Query(sql_handle, StringBuf_Value(&buf))) {
		Sql_ShowDebug(sql_handle);
		StringBuf_Destroy(&buf);
		return NULL;
	}
	StringBuf_Destroy(&buf);

	CREATE(gc, struct guild_castle, 1);
	gc->castle_id = castle_id;

	if (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		Sql_GetData(sql_handle, CD_GUILD_ID, &data, NULL); gc->guild_id =  atoi(data);
		Sql_GetData(sql_handle, CD_CURRENT_ECONOMY, &data, NULL); gc->economy = atoi(data);
		Sql_GetData(sql_handle, CD_CURRENT_DEFENSE, &data, NULL); gc->defense = atoi(data);
		Sql_GetData(sql_handle, CD_INVESTED_ECONOMY, &data, NULL); gc->triggerE = atoi(data);
		Sql_GetData(sql_handle, CD_INVESTED_DEFENSE, &data, NULL); gc->triggerD = atoi(data);
		Sql_GetData(sql_handle, CD_NEXT_TIME, &data, NULL); gc->nextTime = atoi(data);
		Sql_GetData(sql_handle, CD_PAY_TIME, &data, NULL); gc->payTime = atoi(data);
		Sql_GetData(sql_handle, CD_CREATE_TIME, &data, NULL); gc->createTime = atoi(data);
		Sql_GetData(sql_handle, CD_ENABLED_KAFRA, &data, NULL); gc->visibleC = atoi(data);
		for (i = CD_ENABLED_GUARDIAN00; i < CD_MAX; i++) {
			Sql_GetData(sql_handle, i, &data, NULL); gc->guardian[i - CD_ENABLED_GUARDIAN00].visible = atoi(data);
		}
	}
	Sql_FreeResult(sql_handle);

	idb_put(castle_db, castle_id, gc);

	if (charserv_config.save_log)
		ShowInfo("Loaded guild castle (%d - guild %d)\n", castle_id, gc->guild_id);

	return gc;
}


// Read exp_guild.txt
bool exp_guild_parse_row(char* split[], int column, int current)
{
	unsigned int exp = (unsigned int)atol(split[0]);

	if (exp >= UINT_MAX) {
		ShowError("exp_guild: Invalid exp %d at line %d\n", exp, current);
		return false;
	}

	guild_exp[current] = exp;
	return true;
}


int inter_guild_CharOnline(uint32 char_id, int guild_id)
{
	struct guild *g;
	int i;

	if (guild_id == -1) {
		//Get guild_id from the database
		if( SQL_ERROR == Sql_Query(sql_handle, "SELECT guild_id FROM `%s` WHERE char_id='%d'", schema_config.char_db, char_id) )
		{
			Sql_ShowDebug(sql_handle);
			return 0;
		}

		if( SQL_SUCCESS == Sql_NextRow(sql_handle) )
		{
			char* data;

			Sql_GetData(sql_handle, 0, &data, NULL);
			guild_id = atoi(data);
		}
		else
		{
			guild_id = 0;
		}
		Sql_FreeResult(sql_handle);
	}
	if (guild_id == 0)
		return 0; //No guild...

	g = inter_guild_fromsql(guild_id);
	if(!g) {
		ShowError("Character %d's guild %d not found!\n", char_id, guild_id);
		return 0;
	}

	//Member has logged in before saving, tell saver not to delete
	if(g->save_flag & GS_REMOVE)
		g->save_flag &= ~GS_REMOVE;

	//Set member online
	ARR_FIND( 0, g->max_member, i, g->member[i].char_id == char_id );
	if( i < g->max_member )
	{
		g->member[i].online = 1;
		g->member[i].modified = GS_MEMBER_MODIFIED;
	}

	return 1;
}

int inter_guild_CharOffline(uint32 char_id, int guild_id)
{
	struct guild *g=NULL;
	int online_count, i;

	if (guild_id == -1)
	{
		//Get guild_id from the database
		if( SQL_ERROR == Sql_Query(sql_handle, "SELECT guild_id FROM `%s` WHERE char_id='%d'", schema_config.char_db, char_id) )
		{
			Sql_ShowDebug(sql_handle);
			return 0;
		}

		if( SQL_SUCCESS == Sql_NextRow(sql_handle) )
		{
			char* data;

			Sql_GetData(sql_handle, 0, &data, NULL);
			guild_id = atoi(data);
		}
		else
		{
			guild_id = 0;
		}
		Sql_FreeResult(sql_handle);
	}
	if (guild_id == 0)
		return 0; //No guild...

	//Character has a guild, set character offline and check if they were the only member online
	g = inter_guild_fromsql(guild_id);
	if (g == NULL) //Guild not found?
		return 0;

	//Set member offline
	ARR_FIND( 0, g->max_member, i, g->member[i].char_id == char_id );
	if( i < g->max_member )
	{
		g->member[i].online = 0;
		g->member[i].modified = GS_MEMBER_MODIFIED;
	}

	online_count = 0;
	for( i = 0; i < g->max_member; i++ )
		if( g->member[i].online )
			online_count++;

	// Remove guild from memory if no players online
	if( online_count == 0 )
		g->save_flag |= GS_REMOVE;

	return 1;
}

// Initialize guild sql
int inter_guild_sql_init(void)
{
	const char *filename[]={ DBPATH"exp_guild.txt", DBIMPORT"/exp_guild.txt"};
	int i;
	//Initialize the guild cache
	guild_db_= idb_alloc(DB_OPT_RELEASE_DATA);
	castle_db = idb_alloc(DB_OPT_RELEASE_DATA);

	//Read exp file
	for(i = 0; i<ARRAYLENGTH(filename); i++){
		sv_readdb(db_path, filename[i], ',', 1, 1, MAX_GUILDLEVEL, exp_guild_parse_row, i > 0);
	}

	add_timer_func_list(guild_save_timer, "guild_save_timer");
	add_timer(gettick() + 10000, guild_save_timer, 0, 0);
	return 0;
}

/**
 * @see DBApply
 */
int guild_db_final(DBKey key, DBData *data, va_list ap)
{
	struct guild *g = (struct guild*)db_data2ptr(data);
	if (g->save_flag&GS_MASK) {
		inter_guild_tosql(g, g->save_flag&GS_MASK);
		return 1;
	}
	return 0;
}

void inter_guild_sql_final(void)
{
	guild_db_->destroy(guild_db_, guild_db_final);
	db_destroy(castle_db);
	return;
}

// Get guild_id by its name. Returns 0 if not found, -1 on error.
int search_guildname(char *str)
{
	int guild_id;
	char esc_name[NAME_LENGTH*2+1];

	Sql_EscapeStringLen(sql_handle, esc_name, str, safestrnlen(str, NAME_LENGTH));
	//Lookup guilds with the same name
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT guild_id FROM `%s` WHERE name='%s'", schema_config.guild_db, esc_name) )
	{
		Sql_ShowDebug(sql_handle);
		return -1;
	}

	if( SQL_SUCCESS == Sql_NextRow(sql_handle) )
	{
		char* data;

		Sql_GetData(sql_handle, 0, &data, NULL);
		guild_id = atoi(data);
	}
	else
	{
		guild_id = 0;
	}
	Sql_FreeResult(sql_handle);
	return guild_id;
}

// Check if guild is empty
bool guild_check_empty(struct guild *g)
{
	int i;
	ARR_FIND( 0, g->max_member, i, g->member[i].account_id > 0 );
	//Let the calling function handle the guild removal in case they need
	//to do something else with it before freeing the data. [Skotlex]
	return i < g->max_member ? false : true; // not empty
}

unsigned int guild_nextexp(int level)
{
	if (level == 0)
		return 1;
	if (level < 0 || level > MAX_GUILDLEVEL)
		return 0;

	return guild_exp[level-1];
}

int guild_checkskill(struct guild *g,int id)
{
	int idx = id - GD_SKILLBASE;
	return idx < 0 || idx >= MAX_GUILDSKILL ? 0 : g->skill[idx].lv;
}

int guild_calcinfo(struct guild *g)
{
	int i,c;
	unsigned int nextexp;
	struct guild before = *g; // Save guild current values

	if(g->guild_lv<=0)
		g->guild_lv = 1;
	nextexp = guild_nextexp(g->guild_lv);

	// Consume guild exp and increase guild level
	while(g->exp >= nextexp && nextexp > 0){	//fixed guild exp overflow [Kevin]
		g->exp-=nextexp;
		g->guild_lv++;
		g->skill_point++;
		nextexp = guild_nextexp(g->guild_lv);
	}

	// Save next exp step
	g->next_exp = nextexp;

	// Set the max number of members, Guild Extention skill - currently adds 6 to max per skill lv.
	g->max_member = 12 + guild_checkskill(g, GD_EXTENSION) * 2;
	if(g->max_member > MAX_GUILD)
	{
		ShowError("Guild %d:%s has capacity for too many guild members (%d), max supported is %d\n", g->guild_id, g->name, g->max_member, MAX_GUILD);
		g->max_member = MAX_GUILD;
	}

	// Compute the guild average level
	g->average_lv=0;
	g->connect_member=0;
	for(i=c=0;i<g->max_member;i++)
	{
		if(g->member[i].account_id>0)
		{
			if (g->member[i].lv >= 0)
			{
				g->average_lv+=g->member[i].lv;
				c++;
			}
			else
			{
				ShowWarning("Guild %d:%s, member %d:%s has an invalid level %d\n", g->guild_id, g->name, g->member[i].char_id, g->member[i].name, g->member[i].lv);
			}

			if(g->member[i].online)
				g->connect_member++;
		}
	}
	if(c)
		g->average_lv /= c;

	// Check if guild stats has change
	if(g->max_member != before.max_member || g->guild_lv != before.guild_lv || g->skill_point != before.skill_point	)
	{
		g->save_flag |= GS_LEVEL;
		mapif_guild_info(-1,g);
		return 1;
	}

	return 0;
}

//-------------------------------------------------------------------
// Packet sent to map server

int mapif_guild_created(int fd,uint32 account_id,struct guild *g)
{
	WFIFOHEAD(fd, 10);
	WFIFOW(fd,0)=0x3830;
	WFIFOL(fd,2)=account_id;
	if(g != NULL)
	{
		WFIFOL(fd,6)=g->guild_id;
		ShowInfo("int_guild: Guild created (%d - %s)\n",g->guild_id,g->name);
	} else
		WFIFOL(fd,6)=0;

	WFIFOSET(fd,10);
	return 0;
}

// Guild not found
int mapif_guild_noinfo(int fd,int guild_id)
{
	unsigned char buf[12];
	WBUFW(buf,0)=0x3831;
	WBUFW(buf,2)=8;
	WBUFL(buf,4)=guild_id;
	ShowWarning("int_guild: info not found %d\n",guild_id);
	if(fd<0)
		chmapif_sendall(buf,8);
	else
		chmapif_send(fd,buf,8);
	return 0;
}

// Send guild info
int mapif_guild_info(int fd,struct guild *g)
{
	unsigned char buf[8+sizeof(struct guild)];
	WBUFW(buf,0)=0x3831;
	WBUFW(buf,2)=4+sizeof(struct guild);
	memcpy(buf+4,g,sizeof(struct guild));
	if(fd<0)
		chmapif_sendall(buf,WBUFW(buf,2));
	else
		chmapif_send(fd,buf,WBUFW(buf,2));
	return 0;
}

// ACK member add
int mapif_guild_memberadded(int fd,int guild_id,uint32 account_id,uint32 char_id,int flag)
{
	WFIFOHEAD(fd, 15);
	WFIFOW(fd,0)=0x3832;
	WFIFOL(fd,2)=guild_id;
	WFIFOL(fd,6)=account_id;
	WFIFOL(fd,10)=char_id;
	WFIFOB(fd,14)=flag;
	WFIFOSET(fd,15);
	return 0;
}

// ACK member leave
int mapif_guild_withdraw(int guild_id,uint32 account_id,uint32 char_id,int flag, const char *name, const char *mes)
{
	unsigned char buf[55+NAME_LENGTH];
	WBUFW(buf, 0)=0x3834;
	WBUFL(buf, 2)=guild_id;
	WBUFL(buf, 6)=account_id;
	WBUFL(buf,10)=char_id;
	WBUFB(buf,14)=flag;
	memcpy(WBUFP(buf,15),mes,40);
	memcpy(WBUFP(buf,55),name,NAME_LENGTH);
	chmapif_sendall(buf,55+NAME_LENGTH);
	ShowInfo("int_guild: guild withdraw (%d - %d: %s - %s)\n",guild_id,account_id,name,mes);
	return 0;
}

// Send short member's info
int mapif_guild_memberinfoshort(struct guild *g,int idx)
{
	unsigned char buf[19];
	WBUFW(buf, 0)=0x3835;
	WBUFL(buf, 2)=g->guild_id;
	WBUFL(buf, 6)=g->member[idx].account_id;
	WBUFL(buf,10)=g->member[idx].char_id;
	WBUFB(buf,14)=(unsigned char)g->member[idx].online;
	WBUFW(buf,15)=g->member[idx].lv;
	WBUFW(buf,17)=g->member[idx].class_;
	chmapif_sendall(buf,19);
	return 0;
}

// Send guild broken
int mapif_guild_broken(int guild_id,int flag)
{
	unsigned char buf[7];
	WBUFW(buf,0)=0x3836;
	WBUFL(buf,2)=guild_id;
	WBUFB(buf,6)=flag;
	chmapif_sendall(buf,7);
	ShowInfo("int_guild: Guild broken (%d)\n",guild_id);
	return 0;
}

// Send guild message
int mapif_guild_message(int guild_id,uint32 account_id,char *mes,int len, int sfd)
{
	unsigned char buf[512];
	if (len > 500)
		len = 500;
	WBUFW(buf,0)=0x3837;
	WBUFW(buf,2)=len+12;
	WBUFL(buf,4)=guild_id;
	WBUFL(buf,8)=account_id;
	memcpy(WBUFP(buf,12),mes,len);
	chmapif_sendallwos(sfd, buf,len+12);
	return 0;
}

// Send basic info
int mapif_guild_basicinfochanged(int guild_id,int type,const void *data,int len)
{
	unsigned char buf[2048];
	if (len > 2038)
		len = 2038;
	WBUFW(buf, 0)=0x3839;
	WBUFW(buf, 2)=len+10;
	WBUFL(buf, 4)=guild_id;
	WBUFW(buf, 8)=type;
	memcpy(WBUFP(buf,10),data,len);
	chmapif_sendall(buf,len+10);
	return 0;
}

// Send member info
int mapif_guild_memberinfochanged(int guild_id,uint32 account_id,uint32 char_id, int type,const void *data,int len)
{
	unsigned char buf[2048];
	if (len > 2030)
		len = 2030;
	WBUFW(buf, 0)=0x383a;
	WBUFW(buf, 2)=len+18;
	WBUFL(buf, 4)=guild_id;
	WBUFL(buf, 8)=account_id;
	WBUFL(buf,12)=char_id;
	WBUFW(buf,16)=type;
	memcpy(WBUFP(buf,18),data,len);
	chmapif_sendall(buf,len+18);
	return 0;
}

// ACK guild skill up
int mapif_guild_skillupack(int guild_id,uint16 skill_id,uint32 account_id)
{
	unsigned char buf[14];
	WBUFW(buf, 0)=0x383c;
	WBUFL(buf, 2)=guild_id;
	WBUFL(buf, 6)=skill_id;
	WBUFL(buf,10)=account_id;
	chmapif_sendall(buf,14);
	return 0;
}

// ACK guild alliance
int mapif_guild_alliance(int guild_id1,int guild_id2,uint32 account_id1,uint32 account_id2,int flag,const char *name1,const char *name2)
{
	unsigned char buf[19+2*NAME_LENGTH];
	WBUFW(buf, 0)=0x383d;
	WBUFL(buf, 2)=guild_id1;
	WBUFL(buf, 6)=guild_id2;
	WBUFL(buf,10)=account_id1;
	WBUFL(buf,14)=account_id2;
	WBUFB(buf,18)=flag;
	memcpy(WBUFP(buf,19),name1,NAME_LENGTH);
	memcpy(WBUFP(buf,19+NAME_LENGTH),name2,NAME_LENGTH);
	chmapif_sendall(buf,19+2*NAME_LENGTH);
	return 0;
}

// Send a guild position desc
int mapif_guild_position(struct guild *g,int idx)
{
	unsigned char buf[12 + sizeof(struct guild_position)];
	WBUFW(buf,0)=0x383b;
	WBUFW(buf,2)=sizeof(struct guild_position)+12;
	WBUFL(buf,4)=g->guild_id;
	WBUFL(buf,8)=idx;
	memcpy(WBUFP(buf,12),&g->position[idx],sizeof(struct guild_position));
	chmapif_sendall(buf,WBUFW(buf,2));
	return 0;
}

// Send the guild notice
int mapif_guild_notice(struct guild *g)
{
	unsigned char buf[256];
	WBUFW(buf,0)=0x383e;
	WBUFL(buf,2)=g->guild_id;
	memcpy(WBUFP(buf,6),g->mes1,MAX_GUILDMES1);
	memcpy(WBUFP(buf,66),g->mes2,MAX_GUILDMES2);
	chmapif_sendall(buf,186);
	return 0;
}

// Send emblem data
int mapif_guild_emblem(struct guild *g)
{
	unsigned char buf[12 + sizeof(g->emblem_data)];
	WBUFW(buf,0)=0x383f;
	WBUFW(buf,2)=g->emblem_len+12;
	WBUFL(buf,4)=g->guild_id;
	WBUFL(buf,8)=g->emblem_id;
	memcpy(WBUFP(buf,12),g->emblem_data,g->emblem_len);
	chmapif_sendall(buf,WBUFW(buf,2));
	return 0;
}

int mapif_guild_master_changed(struct guild *g, int aid, int cid, time_t time)
{
	unsigned char buf[18];
	WBUFW(buf,0)=0x3843;
	WBUFL(buf,2)=g->guild_id;
	WBUFL(buf,6)=aid;
	WBUFL(buf,10)=cid;
	WBUFL(buf,14)=(uint32)time;
	chmapif_sendall(buf,18);
	return 0;
}

int mapif_guild_castle_dataload(int fd, int sz, int *castle_ids)
{
	struct guild_castle *gc = NULL;
	int num = (sz - 4) / sizeof(int);
	int len = 4 + num * sizeof(*gc);
	int i;

	WFIFOHEAD(fd, len);
	WFIFOW(fd, 0) = 0x3840;
	WFIFOW(fd, 2) = len;
	for (i = 0; i < num; i++) {
		gc = inter_guildcastle_fromsql(*(castle_ids++));
		memcpy(WFIFOP(fd, 4 + i * sizeof(*gc)), gc, sizeof(*gc));
	}
	WFIFOSET(fd, len);
	return 0;
}

//-------------------------------------------------------------------
// Packet received from map server


// Guild creation request
int mapif_parse_CreateGuild(int fd,uint32 account_id,char *name,struct guild_member *master)
{
	struct guild *g;
	int i=0;
#ifdef NOISY
	ShowInfo("Creating Guild (%s)\n", name);
#endif
	if(search_guildname(name) != 0){
		ShowInfo("int_guild: guild with same name exists [%s]\n",name);
		mapif_guild_created(fd,account_id,NULL);
		return 0;
	}
	// Check Authorised letters/symbols in the name of the character
	if (charserv_config.char_config.char_name_option == 1) { // only letters/symbols in char_name_letters are authorised
		for (i = 0; i < NAME_LENGTH && name[i]; i++)
			if (strchr(charserv_config.char_config.char_name_letters, name[i]) == NULL) {
				mapif_guild_created(fd,account_id,NULL);
				return 0;
			}
	} else if (charserv_config.char_config.char_name_option == 2) { // letters/symbols in char_name_letters are forbidden
		for (i = 0; i < NAME_LENGTH && name[i]; i++)
			if (strchr(charserv_config.char_config.char_name_letters, name[i]) != NULL) {
				mapif_guild_created(fd,account_id,NULL);
				return 0;
			}
	}

	g = (struct guild *)aMalloc(sizeof(struct guild));
	memset(g,0,sizeof(struct guild));

	memcpy(g->name,name,NAME_LENGTH);
	memcpy(g->master,master->name,NAME_LENGTH);
	memcpy(&g->member[0],master,sizeof(struct guild_member));
	g->member[0].modified = GS_MEMBER_MODIFIED;

	// Set default positions
	g->position[0].mode = GUILD_PERM_DEFAULT;
	strcpy(g->position[0].name,"GuildMaster");
	strcpy(g->position[MAX_GUILDPOSITION-1].name,"Newbie");
	g->position[0].modified = g->position[MAX_GUILDPOSITION-1].modified = GS_POSITION_MODIFIED;
	for(i=1;i<MAX_GUILDPOSITION-1;i++) {
		sprintf(g->position[i].name,"Position %d",i+1);
		g->position[i].modified = GS_POSITION_MODIFIED;
	}

	// Initialize guild property
	g->max_member=16;
	g->average_lv=master->lv;
	g->connect_member=1;

	for(i=0;i<MAX_GUILDSKILL;i++)
		g->skill[i].id=i + GD_SKILLBASE;
	g->guild_id= -1; //Request to create guild.

	// Create the guild
	if (!inter_guild_tosql(g,GS_BASIC|GS_POSITION|GS_SKILL|GS_MEMBER)) {
		//Failed to Create guild....
		ShowError("Failed to create Guild %s (Guild Master: %s)\n", g->name, g->master);
		mapif_guild_created(fd,account_id,NULL);
		aFree(g);
		return 0;
	}
	ShowInfo("Created Guild %d - %s (Guild Master: %s)\n", g->guild_id, g->name, g->master);

	//Add to cache
	idb_put(guild_db_, g->guild_id, g);

	// Report to client
	mapif_guild_created(fd,account_id,g);
	mapif_guild_info(fd,g);

	if(charserv_config.log_inter)
		inter_log("guild %s (id=%d) created by master %s (id=%d)\n",
			name, g->guild_id, master->name, master->account_id );

	return 0;
}

// Return guild info to client
int mapif_parse_GuildInfo(int fd,int guild_id)
{
	struct guild * g = inter_guild_fromsql(guild_id); //We use this because on start-up the info of castle-owned guilds is requied. [Skotlex]
	if(g)
	{
		if (!guild_calcinfo(g))
			mapif_guild_info(fd,g);
	}
	else
		mapif_guild_noinfo(fd,guild_id); // Failed to load info
	return 0;
}

// Add member to guild
int mapif_parse_GuildAddMember(int fd,int guild_id,struct guild_member *m)
{
	struct guild * g;
	int i;

	g = inter_guild_fromsql(guild_id);
	if(g==NULL){
		// Failed to add
		mapif_guild_memberadded(fd,guild_id,m->account_id,m->char_id,1);
		return 0;
	}

	// Find an empty slot
	for(i=0;i<g->max_member;i++)
	{
		if(g->member[i].account_id==0)
		{
			memcpy(&g->member[i],m,sizeof(struct guild_member));
			g->member[i].modified = (GS_MEMBER_NEW | GS_MEMBER_MODIFIED);
			mapif_guild_memberadded(fd,guild_id,m->account_id,m->char_id,0);
			if (!guild_calcinfo(g)) //Send members if it was not invoked.
				mapif_guild_info(-1,g);

			g->save_flag |= GS_MEMBER;
			if (g->save_flag&GS_REMOVE)
				g->save_flag&=~GS_REMOVE;
			return 0;
		}
	}

	// Failed to add
	mapif_guild_memberadded(fd,guild_id,m->account_id,m->char_id,1);
	return 0;
}

// Delete member from guild
int mapif_parse_GuildLeave(int fd, int guild_id, uint32 account_id, uint32 char_id, int flag, const char *mes)
{
	int i;

	struct guild* g = inter_guild_fromsql(guild_id);
	if( g == NULL )
	{
		// Unknown guild, just update the player
		if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `guild_id`='0' WHERE `account_id`='%d' AND `char_id`='%d'", schema_config.char_db, account_id, char_id) )
			Sql_ShowDebug(sql_handle);
		// mapif_guild_withdraw(guild_id,account_id,char_id,flag,g->member[i].name,mes);
		return 0;
	}

	// Find the member
	ARR_FIND( 0, g->max_member, i, g->member[i].account_id == account_id && g->member[i].char_id == char_id );
	if( i == g->max_member )
	{
		//TODO member not found
		return 0;
	}

	if( flag )
	{	// Write expulsion reason
		// Find an empty slot
		int j;
		ARR_FIND( 0, MAX_GUILDEXPULSION, j, g->expulsion[j].account_id == 0 );
		if( j == MAX_GUILDEXPULSION )
		{
			// Expulsion list is full, flush the oldest one
			for( j = 0; j < MAX_GUILDEXPULSION - 1; j++ )
				g->expulsion[j] = g->expulsion[j+1];
			j = MAX_GUILDEXPULSION-1;
		}
		// Save the expulsion entry
		g->expulsion[j].account_id = account_id;
		safestrncpy(g->expulsion[j].name, g->member[i].name, NAME_LENGTH);
		safestrncpy(g->expulsion[j].mes, mes, 40);
	}

	mapif_guild_withdraw(guild_id,account_id,char_id,flag,g->member[i].name,mes);
	inter_guild_removemember_tosql(g->member[i].char_id);

	memset(&g->member[i],0,sizeof(struct guild_member));

	if( guild_check_empty(g) )
		mapif_parse_BreakGuild(-1,guild_id); //Break the guild.
	else {
		//Update member info.
		if (!guild_calcinfo(g))
			mapif_guild_info(fd,g);
		g->save_flag |= GS_EXPULSION;
	}

	return 0;
}

// Change member info
int mapif_parse_GuildChangeMemberInfoShort(int fd,int guild_id,uint32 account_id,uint32 char_id,int online,int lv,int class_)
{
	// Could speed up by manipulating only guild_member
	struct guild * g;
	int i,sum,c;
	int prev_count, prev_alv;

	g = inter_guild_fromsql(guild_id);
	if(g==NULL)
		return 0;

	ARR_FIND( 0, g->max_member, i, g->member[i].account_id == account_id && g->member[i].char_id == char_id );
	if( i < g->max_member )
	{
		g->member[i].online = online;
		g->member[i].lv = lv;
		g->member[i].class_ = class_;
		g->member[i].modified = GS_MEMBER_MODIFIED;
		mapif_guild_memberinfoshort(g,i);
	}

	prev_count = g->connect_member;
	prev_alv = g->average_lv;

	g->average_lv = 0;
	g->connect_member = 0;
	c = 0;
	sum = 0;

	for( i = 0; i < g->max_member; i++ )
	{
		if( g->member[i].account_id > 0 )
		{
			sum += g->member[i].lv;
			c++;
		}
		if( g->member[i].online )
			g->connect_member++;
	}

	if( c ) // this check should always succeed...
	{
		g->average_lv = sum / c;
		if( g->connect_member != prev_count || g->average_lv != prev_alv )
			g->save_flag |= GS_CONNECT;
		if( g->save_flag & GS_REMOVE )
			g->save_flag &= ~GS_REMOVE;
	}
	g->save_flag |= GS_MEMBER; //Update guild member data
	return 0;
}

// BreakGuild
int mapif_parse_BreakGuild(int fd,int guild_id)
{
	struct guild * g;

	g = inter_guild_fromsql(guild_id);
	if(g==NULL)
		return 0;

	// Delete guild from sql
	//printf("- Delete guild %d from guild\n",guild_id);
	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id` = '%d'", schema_config.guild_db, guild_id) )
		Sql_ShowDebug(sql_handle);

	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id` = '%d'", schema_config.guild_member_db, guild_id) )
		Sql_ShowDebug(sql_handle);

	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id` = '%d'", schema_config.guild_castle_db, guild_id) )
		Sql_ShowDebug(sql_handle);

	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id` = '%d'", schema_config.guild_storage_db, guild_id) )
		Sql_ShowDebug(sql_handle);

	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id` = '%d' OR `alliance_id` = '%d'", schema_config.guild_alliance_db, guild_id, guild_id) )
		Sql_ShowDebug(sql_handle);

	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id` = '%d'", schema_config.guild_position_db, guild_id) )
		Sql_ShowDebug(sql_handle);

	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id` = '%d'", schema_config.guild_skill_db, guild_id) )
		Sql_ShowDebug(sql_handle);

	if( SQL_ERROR == Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `guild_id` = '%d'", schema_config.guild_expulsion_db, guild_id) )
		Sql_ShowDebug(sql_handle);

	//printf("- Update guild %d of char\n",guild_id);
	if( SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `guild_id`='0' WHERE `guild_id`='%d'", schema_config.char_db, guild_id) )
		Sql_ShowDebug(sql_handle);

	mapif_guild_broken(guild_id,0);

	if(charserv_config.log_inter)
		inter_log("guild %s (id=%d) broken\n",g->name,guild_id);

	//Remove the guild from memory. [Skotlex]
	idb_remove(guild_db_, guild_id);
	return 0;
}

// Forward Guild message to others map servers
int mapif_parse_GuildMessage(int fd,int guild_id,uint32 account_id,char *mes,int len)
{
	return mapif_guild_message(guild_id,account_id,mes,len, fd);
}

// Modification of the guild
int mapif_parse_GuildBasicInfoChange(int fd,int guild_id,int type,const char *data,int len)
{
	struct guild *g = inter_guild_fromsql(guild_id);

	if (!g)
		return 0;

	short data_value = *((short *)data);

	switch(type) {
		case GBI_GUILDLV:
			if (data_value > 0 && g->guild_lv + data_value <= MAX_GUILDLEVEL) {
				g->guild_lv += data_value;
				g->skill_point += data_value;
			} else if (data_value < 0 && g->guild_lv + data_value >= 1)
				g->guild_lv += data_value;

			mapif_guild_info(-1, g);
			g->save_flag |= GS_LEVEL;
			return 0;
		default:
			ShowError("int_guild: GuildBasicInfoChange: Unknown type %d\n",type);
			break;
	}
	mapif_guild_basicinfochanged(guild_id,type,data,len);
	return 0;
}

/**
 * Receive a modification request for the guildmember
 * @param fd : map-serv link
 * @param guild_id : Guild to alter
 * @param account_id : Player aid to alter 
 * @param char_id : Player cid to alter
 * @param type : Type of modification
 * @param data : Value of modification
 * @param len : Size of value
 * @return 
 */
int mapif_parse_GuildMemberInfoChange(int fd,int guild_id,uint32 account_id,uint32 char_id,int type,const char *data,int len)
{
	// Could make some improvement in speed, because only change guild_member
	int i;
	struct guild * g;

	g = inter_guild_fromsql(guild_id);
	if(g==NULL)
		return 0;

	// Search the member
	for(i=0;i<g->max_member;i++)
		if(	g->member[i].account_id==account_id &&
			g->member[i].char_id==char_id )
				break;

	// Not Found
	if(i==g->max_member){
		ShowWarning("int_guild: GuildMemberChange: Not found %d,%d in guild (%d - %s)\n",
			account_id,char_id,guild_id,g->name);
		return 0;
	}

	switch(type)
	{
		case GMI_POSITION:
		  {
			g->member[i].position=*((short *)data);
			g->member[i].modified = GS_MEMBER_MODIFIED;
			mapif_guild_memberinfochanged(guild_id,account_id,char_id,type,data,len);
			g->save_flag |= GS_MEMBER;
			break;
		  }
		case GMI_EXP:
		{	// EXP
			uint64 old_exp=g->member[i].exp;
			g->member[i].exp=*((uint64 *)data);
			g->member[i].modified = GS_MEMBER_MODIFIED;
			if (g->member[i].exp > old_exp)
			{
				uint64 exp = g->member[i].exp - old_exp;

				// Compute gained exp
				if (charserv_config.guild_exp_rate != 100)
					exp = exp*(charserv_config.guild_exp_rate)/100;

				// Update guild exp
				if (exp > UINT64_MAX - g->exp)
					g->exp = UINT64_MAX;
				else
					g->exp+=exp;

				guild_calcinfo(g);
				mapif_guild_basicinfochanged(guild_id,GBI_EXP,&g->exp,sizeof(g->exp));
				g->save_flag |= GS_LEVEL;
			}
			mapif_guild_memberinfochanged(guild_id,account_id,char_id,type,data,len);
			g->save_flag |= GS_MEMBER;
			break;
		}
		case GMI_HAIR:
		{
			g->member[i].hair=*((short *)data);
			g->member[i].modified = GS_MEMBER_MODIFIED;
			mapif_guild_memberinfochanged(guild_id,account_id,char_id,type,data,len);
			g->save_flag |= GS_MEMBER; //Save new data.
			break;
		}
		case GMI_HAIR_COLOR:
		{
			g->member[i].hair_color=*((short *)data);
			g->member[i].modified = GS_MEMBER_MODIFIED;
			mapif_guild_memberinfochanged(guild_id,account_id,char_id,type,data,len);
			g->save_flag |= GS_MEMBER; //Save new data.
			break;
		}
		case GMI_GENDER:
		{
			g->member[i].gender=*((short *)data);
			g->member[i].modified = GS_MEMBER_MODIFIED;
			mapif_guild_memberinfochanged(guild_id,account_id,char_id,type,data,len);
			g->save_flag |= GS_MEMBER; //Save new data.
			break;
		}
		case GMI_CLASS:
		{
			g->member[i].class_=*((short *)data);
			g->member[i].modified = GS_MEMBER_MODIFIED;
			mapif_guild_memberinfochanged(guild_id,account_id,char_id,type,data,len);
			g->save_flag |= GS_MEMBER; //Save new data.
			break;
		}
		case GMI_LEVEL:
		{
			g->member[i].lv=*((short *)data);
			g->member[i].modified = GS_MEMBER_MODIFIED;
			mapif_guild_memberinfochanged(guild_id,account_id,char_id,type,data,len);
			g->save_flag |= GS_MEMBER; //Save new data.
			break;
		}
		default:
		  ShowError("int_guild: GuildMemberInfoChange: Unknown type %d\n",type);
		  break;
	}
	return 0;
}

int inter_guild_sex_changed(int guild_id,uint32 account_id,uint32 char_id, short gender)
{
	return mapif_parse_GuildMemberInfoChange(0, guild_id, account_id, char_id, GMI_GENDER, (const char*)&gender, sizeof(gender));
}

int inter_guild_charname_changed(int guild_id,uint32 account_id, uint32 char_id, char *name)
{
	struct guild *g;
	int i, flag = 0;

	g = inter_guild_fromsql(guild_id);
	if( g == NULL )
	{
		ShowError("inter_guild_charrenamed: Can't find guild %d.\n", guild_id);
		return 0;
	}

	ARR_FIND(0, g->max_member, i, g->member[i].char_id == char_id);
	if( i == g->max_member )
	{
		ShowError("inter_guild_charrenamed: Can't find character %d in the guild\n", char_id);
		return 0;
	}

	if( !strcmp(g->member[i].name, g->master) )
	{
		safestrncpy(g->master, name, NAME_LENGTH);
		flag |= GS_BASIC;
	}
	safestrncpy(g->member[i].name, name, NAME_LENGTH);
	g->member[i].modified = GS_MEMBER_MODIFIED;
	flag |= GS_MEMBER;

	if( !inter_guild_tosql(g, flag) )
		return 0;

	mapif_guild_info(-1,g);

	return 0;
}

// Change a position desc
int mapif_parse_GuildPosition(int fd,int guild_id,int idx,struct guild_position *p)
{
	// Could make some improvement in speed, because only change guild_position
	struct guild * g;

	g = inter_guild_fromsql(guild_id);
	if(g==NULL || idx<0 || idx>=MAX_GUILDPOSITION)
		return 0;

	memcpy(&g->position[idx],p,sizeof(struct guild_position));
	mapif_guild_position(g,idx);
	g->position[idx].modified = GS_POSITION_MODIFIED;
	g->save_flag |= GS_POSITION; // Change guild_position
	return 0;
}

// Guild Skill UP
int mapif_parse_GuildSkillUp(int fd,int guild_id,uint16 skill_id,uint32 account_id,int max)
{
	struct guild * g;
	int idx = skill_id - GD_SKILLBASE;

	g = inter_guild_fromsql(guild_id);
	if(g == NULL || idx < 0 || idx >= MAX_GUILDSKILL)
		return 0;

	if(g->skill_point>0 && g->skill[idx].id>0 && g->skill[idx].lv<max )
	{
		g->skill[idx].lv++;
		g->skill_point--;
		if (!guild_calcinfo(g))
			mapif_guild_info(-1,g);
		mapif_guild_skillupack(guild_id,skill_id,account_id);
		g->save_flag |= (GS_LEVEL|GS_SKILL); // Change guild & guild_skill
		if (skill_id == GD_GUILD_STORAGE)
			inter_guild_tosql(g, g->save_flag); // Force save for GD_GUILD_STORAGE
	}
	return 0;
}

//Manual deletion of an alliance when partnering guild does not exists. [Skotlex]
int mapif_parse_GuildDeleteAlliance(struct guild *g, int guild_id, uint32 account_id1, uint32 account_id2, int flag)
{
	int i;
	char name[NAME_LENGTH];

	ARR_FIND( 0, MAX_GUILDALLIANCE, i, g->alliance[i].guild_id == guild_id );
	if( i == MAX_GUILDALLIANCE )
		return -1;

	strcpy(name, g->alliance[i].name);
	g->alliance[i].guild_id=0;

	mapif_guild_alliance(g->guild_id,guild_id,account_id1,account_id2,flag,g->name,name);
	g->save_flag |= GS_ALLIANCE;
	return 0;
}

/**
 * Alliance modification
 * @param fd
 * @param guild_id1
 * @param guild_id2
 * @param account_id1
 * @param account_id2
 * @param flag
 * @return 
 */
int mapif_parse_GuildAlliance(int fd,int guild_id1,int guild_id2,uint32 account_id1,uint32 account_id2,int flag)
{
	// Could speed up
	struct guild *g[2];
	int j,i;
	g[0] = inter_guild_fromsql(guild_id1);
	g[1] = inter_guild_fromsql(guild_id2);

	if(g[0] && g[1]==NULL && (flag & GUILD_ALLIANCE_REMOVE)) //Requested to remove an alliance with a not found guild.
		return mapif_parse_GuildDeleteAlliance(g[0], guild_id2,	account_id1, account_id2, flag); //Try to do a manual removal of said guild.

	if(g[0]==NULL || g[1]==NULL)
		return 0;

	if(flag&GUILD_ALLIANCE_REMOVE)
	{
		// Remove alliance/opposition, in case of alliance, remove on both side
		for(i=0;i<2-(flag&GUILD_ALLIANCE_TYPE_MASK);i++)
		{
			ARR_FIND( 0, MAX_GUILDALLIANCE, j, g[i]->alliance[j].guild_id == g[1-i]->guild_id && g[i]->alliance[j].opposition == (flag&GUILD_ALLIANCE_TYPE_MASK) );
			if( j < MAX_GUILDALLIANCE )
				g[i]->alliance[j].guild_id = 0;
		}
	}
	else
	{
		// Add alliance, in case of alliance, add on both side
		for(i=0;i<2-(flag&GUILD_ALLIANCE_TYPE_MASK);i++)
		{
			// Search an empty slot
			ARR_FIND( 0, MAX_GUILDALLIANCE, j, g[i]->alliance[j].guild_id == 0 );
			if( j < MAX_GUILDALLIANCE )
			{
				g[i]->alliance[j].guild_id=g[1-i]->guild_id;
				memcpy(g[i]->alliance[j].name,g[1-i]->name,NAME_LENGTH);
				// Set alliance type
				g[i]->alliance[j].opposition = flag&GUILD_ALLIANCE_TYPE_MASK;
			}
		}
	}

	// Send on all map the new alliance/opposition
	mapif_guild_alliance(guild_id1,guild_id2,account_id1,account_id2,flag,g[0]->name,g[1]->name);

	// Mark the two guild to be saved
	g[0]->save_flag |= GS_ALLIANCE;
	g[1]->save_flag |= GS_ALLIANCE;
	return 1;
}

// Change guild message
int mapif_parse_GuildNotice(int fd,int guild_id,const char *mes1,const char *mes2)
{
	struct guild *g;

	g = inter_guild_fromsql(guild_id);
	if(g==NULL)
		return 0;

	memcpy(g->mes1,mes1,MAX_GUILDMES1);
	memcpy(g->mes2,mes2,MAX_GUILDMES2);
	g->save_flag |= GS_MES;	//Change mes of guild
	inter_guild_tosql(g, g->save_flag);
	return mapif_guild_notice(g);
}

int mapif_parse_GuildEmblem(int fd,int len,int guild_id,int dummy,const char *data)
{
	struct guild * g;

	g = inter_guild_fromsql(guild_id);
	if(g==NULL)
		return 0;

	if (len > sizeof(g->emblem_data))
		len = sizeof(g->emblem_data);

	memcpy(g->emblem_data,data,len);
	g->emblem_len=len;
	g->emblem_id++;
	g->save_flag |= GS_EMBLEM;	//Change guild
	return mapif_guild_emblem(g);
}

int mapif_parse_GuildCastleDataLoad(int fd, int len, int *castle_ids)
{
	return mapif_guild_castle_dataload(fd, len, castle_ids);
}

int mapif_parse_GuildCastleDataSave(int fd, int castle_id, int index, int value)
{
	struct guild_castle *gc = inter_guildcastle_fromsql(castle_id);

	if (gc == NULL) {
		ShowError("mapif_parse_GuildCastleDataSave: castle id=%d not found\n", castle_id);
		return 0;
	}

	switch (index) {
		case CD_GUILD_ID:
			if (charserv_config.log_inter && gc->guild_id != value) {
				int gid = (value) ? value : gc->guild_id;
				struct guild *g = (struct guild*)idb_get(guild_db_, gid);
				inter_log("guild %s (id=%d) %s castle id=%d\n",
				          (g) ? g->name : "??", gid, (value) ? "occupy" : "abandon", castle_id);
			}
			gc->guild_id = value;
			break;
		case CD_CURRENT_ECONOMY: gc->economy = value; break;
		case CD_CURRENT_DEFENSE: gc->defense = value; break;
		case CD_INVESTED_ECONOMY: gc->triggerE = value; break;
		case CD_INVESTED_DEFENSE: gc->triggerD = value; break;
		case CD_NEXT_TIME: gc->nextTime = value; break;
		case CD_PAY_TIME: gc->payTime = value; break;
		case CD_CREATE_TIME: gc->createTime = value; break;
		case CD_ENABLED_KAFRA: gc->visibleC = value; break;
		default:
			if (index >= CD_ENABLED_GUARDIAN00 && index < CD_MAX) {
				gc->guardian[index - CD_ENABLED_GUARDIAN00].visible = value;
				break;
			}
			ShowError("mapif_parse_GuildCastleDataSave: not found index=%d\n", index);
			return 0;
	}
	inter_guildcastle_tosql(gc);
	return 0;
}

int mapif_parse_GuildMasterChange(int fd, int guild_id, const char* name, int len)
{
	struct guild * g;
	struct guild_member gm;
	int pos;

	g = inter_guild_fromsql(guild_id);

	if(g==NULL || len > NAME_LENGTH)
		return 0;

	// Find member (name)
	for (pos = 0; pos < g->max_member && strncmp(g->member[pos].name, name, len); pos++);

	if (pos == g->max_member)
		return 0; //Character not found??

	// Switch current and old GM
	memcpy(&gm, &g->member[pos], sizeof (struct guild_member));
	memcpy(&g->member[pos], &g->member[0], sizeof(struct guild_member));
	memcpy(&g->member[0], &gm, sizeof(struct guild_member));

	// Switch positions
	g->member[pos].position = g->member[0].position;
	g->member[pos].modified = GS_MEMBER_MODIFIED;
	g->member[0].position = 0; //Position 0: guild Master.
	g->member[0].modified = GS_MEMBER_MODIFIED;

	// Store changing time
	g->last_leader_change = time(NULL);

	safestrncpy(g->master, name, len);
	if (len < NAME_LENGTH)
		g->master[len] = '\0';

	ShowInfo("int_guild: Guildmaster Changed to %s (Guild %d - %s)\n",g->master, guild_id, g->name);
	g->save_flag |= (GS_BASIC|GS_MEMBER); //Save main data and member data.
	return mapif_guild_master_changed(g, g->member[0].account_id, g->member[0].char_id, g->last_leader_change);
}

// Communication from the map server
// - Can analyzed only one by one packet
// Data packet length that you set to inter.cpp
//- Shouldn't do checking and packet length, RFIFOSKIP is done by the caller
// Must Return
//	1 : ok
//  0 : error
int inter_guild_parse_frommap(int fd)
{
	RFIFOHEAD(fd);
	switch(RFIFOW(fd,0)) {
	case 0x3030: mapif_parse_CreateGuild(fd,RFIFOL(fd,4),RFIFOCP(fd,8),(struct guild_member *)RFIFOP(fd,32)); break;
	case 0x3031: mapif_parse_GuildInfo(fd,RFIFOL(fd,2)); break;
	case 0x3032: mapif_parse_GuildAddMember(fd,RFIFOL(fd,4),(struct guild_member *)RFIFOP(fd,8)); break;
	case 0x3033: mapif_parse_GuildMasterChange(fd,RFIFOL(fd,4),RFIFOCP(fd,8),RFIFOW(fd,2)-8); break;
	case 0x3034: mapif_parse_GuildLeave(fd,RFIFOL(fd,2),RFIFOL(fd,6),RFIFOL(fd,10),RFIFOB(fd,14),RFIFOCP(fd,15)); break;
	case 0x3035: mapif_parse_GuildChangeMemberInfoShort(fd,RFIFOL(fd,2),RFIFOL(fd,6),RFIFOL(fd,10),RFIFOB(fd,14),RFIFOW(fd,15),RFIFOW(fd,17)); break;
	case 0x3036: mapif_parse_BreakGuild(fd,RFIFOL(fd,2)); break;
	case 0x3037: mapif_parse_GuildMessage(fd,RFIFOL(fd,4),RFIFOL(fd,8),RFIFOCP(fd,12),RFIFOW(fd,2)-12); break;
	case 0x3039: mapif_parse_GuildBasicInfoChange(fd,RFIFOL(fd,4),RFIFOW(fd,8),RFIFOCP(fd,10),RFIFOW(fd,2)-10); break;
	case 0x303A: mapif_parse_GuildMemberInfoChange(fd,RFIFOL(fd,4),RFIFOL(fd,8),RFIFOL(fd,12),RFIFOW(fd,16),RFIFOCP(fd,18),RFIFOW(fd,2)-18); break;
	case 0x303B: mapif_parse_GuildPosition(fd,RFIFOL(fd,4),RFIFOL(fd,8),(struct guild_position *)RFIFOP(fd,12)); break;
	case 0x303C: mapif_parse_GuildSkillUp(fd,RFIFOL(fd,2),RFIFOL(fd,6),RFIFOL(fd,10),RFIFOL(fd,14)); break;
	case 0x303D: mapif_parse_GuildAlliance(fd,RFIFOL(fd,2),RFIFOL(fd,6),RFIFOL(fd,10),RFIFOL(fd,14),RFIFOB(fd,18)); break;
	case 0x303E: mapif_parse_GuildNotice(fd,RFIFOL(fd,2),RFIFOCP(fd,6),RFIFOCP(fd,66)); break;
	case 0x303F: mapif_parse_GuildEmblem(fd,RFIFOW(fd,2)-12,RFIFOL(fd,4),RFIFOL(fd,8),RFIFOCP(fd,12)); break;
	case 0x3040: mapif_parse_GuildCastleDataLoad(fd,RFIFOW(fd,2),(int *)RFIFOP(fd,4)); break;
	case 0x3041: mapif_parse_GuildCastleDataSave(fd,RFIFOW(fd,2),RFIFOB(fd,4),RFIFOL(fd,5)); break;

	default:
		return 0;
	}

	return 1;
}

//Leave request from the server (for deleting character from guild)
int inter_guild_leave(int guild_id, uint32 account_id, uint32 char_id)
{
	return mapif_parse_GuildLeave(-1, guild_id, account_id, char_id, 0, "** Character Deleted **");
}

int inter_guild_broken(int guild_id)
{
	return mapif_guild_broken(guild_id, 0);
}

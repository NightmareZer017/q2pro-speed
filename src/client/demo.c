/*
Copyright (C) 2003-2006 Andrey Nazarov

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

//
// cl_demo.c - demo recording and playback
//

#include "client.h"

static byte     demo_buffer[MAX_PACKETLEN];

static cvar_t   *cl_demosnaps;
static cvar_t   *cl_demomsglen;
static cvar_t   *cl_demowait;
static cvar_t   *cl_demoformat;

static usercmd_t demo_next_cmd;
static int demo_next_cmd_time;
static int demo_outgoing_sequence;

enum {
    dem_usercmd,
    dem_server_message
} demoMessageType_t;

// =========================================================================

/*
====================
CL_WriteDemoMessage

Dumps the current demo message, prefixed by the length.
Stops demo recording and returns false on write error.
====================
*/
bool CL_WriteDemoMessage(sizebuf_t *buf)
{
    uint32_t msglen;
    int ret;

    if (buf->overflowed) {
        SZ_Clear(buf);
        Com_WPrintf("Demo message overflowed (should never happen).\n");
        return true;
    }

    if (!buf->cursize)
        return true;

    if (cls.demo.recording_type == DEM_PREDICTION_EXTENSION) {
        const byte message_type = dem_server_message;
        FS_Write(&message_type, sizeof(message_type), cls.demo.recording);

        // extra info for prediction extension: the number of the command that
        // the server has acknowledged already, so that we know which commands
        // to include in the prediction
        const int ack = cls.netchan->incoming_acknowledged;
        const unsigned cmd_acknowledged = cl.history[ack & CMD_MASK].cmdNumber;
        FS_Write(&cmd_acknowledged, sizeof(cmd_acknowledged), cls.demo.recording);

        // extra info for prediction extension: how long has the current cmd
        // (that has not been saved yet) been cooked for? We need this to time
        // the prediction correctly with respect to the server frames.
        // Theoretically it might be enough to send this only once at the start,
        // but one byte should not hurt and will make sure that we are always
        // synced up.
        const byte current_msec = cl.cmd.msec;
        FS_Write(&current_msec, sizeof(current_msec), cls.demo.recording);
    }

    msglen = LittleLong(buf->cursize);
    ret = FS_Write(&msglen, 4, cls.demo.recording);
    if (ret != 4)
        goto fail;
    ret = FS_Write(buf->data, buf->cursize, cls.demo.recording);
    if (ret != buf->cursize)
        goto fail;

    Com_DDPrintf("%s: wrote %"PRIz" bytes\n", __func__, buf->cursize);

    SZ_Clear(buf);
    return true;

fail:
    SZ_Clear(buf);
    Com_EPrintf("Couldn't write demo: %s\n", Q_ErrorString(ret));
    CL_Stop_f();
    return false;
}

/*
====================
CL_WriteDemoCmd
====================
*/
bool CL_WriteDemoCmd(void)
{
    int		i;
    usercmd_t cmd;

    const byte message_type = dem_usercmd;
    FS_Write(&message_type, sizeof(message_type), cls.demo.recording);

    unsigned cmd_number = LittleLong(cl.cmdNumber);
    FS_Write(&cmd_number, sizeof(cmd_number), cls.demo.recording);

    cmd = cl.cmd;
    for (i = 0; i < 3; i++)
        cmd.angles[i] = LittleShort(cmd.angles[i]);
    cmd.forwardmove = LittleShort(cmd.forwardmove);
    cmd.sidemove    = LittleShort(cmd.sidemove);
    cmd.upmove      = LittleShort(cmd.upmove);

    FS_Write(&cmd, sizeof(cmd), cls.demo.recording);

    return true;
}

// writes a delta update of an entity_state_t list to the message.
static void emit_packet_entities(server_frame_t *from, server_frame_t *to)
{
    entity_packed_t oldpack, newpack;
    entity_state_t *oldent, *newent;
    int     oldindex, newindex;
    int     oldnum, newnum;
    int     i, from_num_entities;

    if (!from)
        from_num_entities = 0;
    else
        from_num_entities = from->numEntities;

    newindex = 0;
    oldindex = 0;
    oldent = newent = NULL;
    while (newindex < to->numEntities || oldindex < from_num_entities) {
        if (newindex >= to->numEntities) {
            newnum = 9999;
        } else {
            i = (to->firstEntity + newindex) & PARSE_ENTITIES_MASK;
            newent = &cl.entityStates[i];
            newnum = newent->number;
        }

        if (oldindex >= from_num_entities) {
            oldnum = 9999;
        } else {
            i = (from->firstEntity + oldindex) & PARSE_ENTITIES_MASK;
            oldent = &cl.entityStates[i];
            oldnum = oldent->number;
        }

        if (newnum == oldnum) {
            // Delta update from old position. Because the force parm is false,
            // this will not result in any bytes being emitted if the entity has
            // not changed at all. Note that players are always 'newentities',
            // this updates their old_origin always and prevents warping in case
            // of packet loss.
            MSG_PackEntity(&oldpack, oldent, false);
            MSG_PackEntity(&newpack, newent, false);
            msgEsFlags_t flags = newent->number <= cl.maxclients ? MSG_ES_NEWENTITY : 0;
            if (cls.demo.recording_type == DEM_PREDICTION_EXTENSION) {
                flags |= cl.esFlags & MSG_ES_LONGSOLID;
            }
            MSG_WriteDeltaEntity(&oldpack, &newpack, flags);
            oldindex++;
            newindex++;
            continue;
        }

        if (newnum < oldnum) {
            // this is a new entity, send it from the baseline
            MSG_PackEntity(&oldpack, &cl.baselines[newnum], false);
            MSG_PackEntity(&newpack, newent, false);
            msgEsFlags_t flags = MSG_ES_FORCE | MSG_ES_NEWENTITY;
            if (cls.demo.recording_type == DEM_PREDICTION_EXTENSION) {
                flags |= cl.esFlags & MSG_ES_LONGSOLID;
            }
            MSG_WriteDeltaEntity(&oldpack, &newpack, flags);
            newindex++;
            continue;
        }

        if (newnum > oldnum) {
            // the old entity isn't present in the new message
            MSG_PackEntity(&oldpack, oldent, false);
            msgEsFlags_t flags = MSG_ES_FORCE;
            if (cls.demo.recording_type == DEM_PREDICTION_EXTENSION) {
                flags |= cl.esFlags & MSG_ES_LONGSOLID;
            }
            MSG_WriteDeltaEntity(&oldpack, NULL, flags);
            oldindex++;
            continue;
        }
    }

    MSG_WriteShort(0);      // end of packetentities
}

static void emit_delta_frame(server_frame_t *from, server_frame_t *to,
                             int fromnum, int tonum)
{
    player_packed_t oldpack, newpack;

    MSG_WriteByte(svc_frame);
    MSG_WriteLong(tonum);
    MSG_WriteLong(fromnum);   // what we are delta'ing from
    MSG_WriteByte(0);   // rate dropped packets

    // send over the areabits
    MSG_WriteByte(to->areabytes);
    MSG_WriteData(to->areabits, to->areabytes);

    // delta encode the playerstate
    MSG_WriteByte(svc_playerinfo);
    MSG_PackPlayer(&newpack, &to->ps);
    if (from) {
        MSG_PackPlayer(&oldpack, &from->ps);
        MSG_WriteDeltaPlayerstate_Default(&oldpack, &newpack);
    } else {
        MSG_WriteDeltaPlayerstate_Default(NULL, &newpack);
    }

    // delta encode the entities
    MSG_WriteByte(svc_packetentities);
    emit_packet_entities(from, to);
}

// frames_written counter starts at 0, but we add 1 to every frame number
// because frame 0 can't be used due to protocol limitation (hack).
#define FRAME_PRE   (cls.demo.frames_written)
#define FRAME_CUR   (cls.demo.frames_written + 1)

/*
====================
CL_EmitDemoFrame

Writes delta from the last frame we got to the current frame.
====================
*/
void CL_EmitDemoFrame(void)
{
    server_frame_t  *oldframe;
    int             lastframe;

    if (!cl.frame.valid)
        return;

    // the first frame is delta uncompressed
    if (cls.demo.last_server_frame == -1) {
        oldframe = NULL;
        lastframe = -1;
    } else {
        oldframe = &cl.frames[cls.demo.last_server_frame & UPDATE_MASK];
        lastframe = FRAME_PRE;
        if (oldframe->number != cls.demo.last_server_frame || !oldframe->valid ||
            cl.numEntityStates - oldframe->firstEntity > MAX_PARSE_ENTITIES) {
            oldframe = NULL;
            lastframe = -1;
        }
    }

    // emit and flush frame
    emit_delta_frame(oldframe, &cl.frame, lastframe, FRAME_CUR);

    if (cls.demo.buffer.cursize + msg_write.cursize > cls.demo.buffer.maxsize) {
        Com_DPrintf("Demo frame overflowed (%"PRIz" + %"PRIz" > %"PRIz")\n",
                    cls.demo.buffer.cursize, msg_write.cursize, cls.demo.buffer.maxsize);
        cls.demo.frames_dropped++;

        // warn the user if drop rate is too high
        if (cls.demo.frames_written < 10 && cls.demo.frames_dropped == 50)
            Com_WPrintf("Too many demo frames don't fit into %"PRIz" bytes.\n"
                        "Try to increase 'cl_demomsglen' value and restart recording.\n",
                        cls.demo.buffer.maxsize);
    } else {
        SZ_Write(&cls.demo.buffer, msg_write.data, msg_write.cursize);
        cls.demo.last_server_frame = cl.frame.number;
        cls.demo.frames_written++;
    }

    SZ_Clear(&msg_write);
}

static size_t format_demo_size(char *buffer, size_t size)
{
    return Com_FormatSizeLong(buffer, size, FS_Tell(cls.demo.recording));
}

static size_t format_demo_status(char *buffer, size_t size)
{
    size_t len = format_demo_size(buffer, size);
    int min, sec, frames = cls.demo.frames_written;

    sec = frames / 10; frames %= 10;
    min = sec / 60; sec %= 60;

    len += Q_scnprintf(buffer + len, size - len, ", %d:%02d.%d",
                       min, sec, frames);

    if (cls.demo.frames_dropped) {
        len += Q_scnprintf(buffer + len, size - len, ", %d frame%s dropped",
                           cls.demo.frames_dropped,
                           cls.demo.frames_dropped == 1 ? "" : "s");
    }

    if (cls.demo.others_dropped) {
        len += Q_scnprintf(buffer + len, size - len, ", %d message%s dropped",
                           cls.demo.others_dropped,
                           cls.demo.others_dropped == 1 ? "" : "s");
    }

    return len;
}

/*
====================
CL_Stop_f

stop recording a demo
====================
*/
void CL_Stop_f(void)
{
    uint32_t msglen;
    char buffer[MAX_QPATH];

    if (!cls.demo.recording) {
        Com_Printf("Not recording a demo.\n");
        return;
    }

// finish up
    if (cls.demo.recording_type == DEM_PREDICTION_EXTENSION) {
        const byte message_type = dem_server_message;
        FS_Write(&message_type, sizeof(message_type), cls.demo.recording);

        const unsigned cmd_acknowledged = 0;
        FS_Write(&cmd_acknowledged, sizeof(cmd_acknowledged), cls.demo.recording);

        const byte current_msec = 0;
        FS_Write(&current_msec, sizeof(current_msec), cls.demo.recording);
    }
    msglen = (uint32_t)-1;
    FS_Write(&msglen, 4, cls.demo.recording);

    format_demo_size(buffer, sizeof(buffer));

// close demofile
    FS_FCloseFile(cls.demo.recording);
    cls.demo.recording = 0;
    cls.demo.paused = false;
    cls.demo.frames_written = 0;
    cls.demo.frames_dropped = 0;
    cls.demo.others_dropped = 0;

// print some statistics
    Com_Printf("Stopped demo (%s).\n", buffer);

// tell the server we finished recording
    CL_UpdateRecordingSetting();
}

static const cmd_option_t o_record[] = {
    { "h", "help", "display this message" },
    { "z", "compress", "compress demo with gzip" },
    { "e", "extended", "use extended packet size" },
    { "s", "standard", "use standard packet size" },
    { NULL }
};

/*
====================
CL_Record_f

record <demoname>

Begins recording a demo from the current position
====================
*/
static void CL_Record_f(void)
{
    char    buffer[MAX_OSPATH];
    int     i, c;
    size_t  len;
    entity_state_t  *ent;
    entity_packed_t pack;
    char            *s;
    qhandle_t       f;
    unsigned        mode = FS_MODE_WRITE;
    size_t          size = Cvar_ClampInteger(
                               cl_demomsglen,
                               MIN_PACKETLEN,
                               MAX_PACKETLEN_WRITABLE);

    while ((c = Cmd_ParseOptions(o_record)) != -1) {
        switch (c) {
        case 'h':
            Cmd_PrintUsage(o_record, "<filename>");
            Com_Printf("Begin client demo recording.\n");
            Cmd_PrintHelp(o_record);
            return;
        case 'z':
            mode |= FS_FLAG_GZIP;
            break;
        case 'e':
            size = MAX_PACKETLEN_WRITABLE;
            break;
        case 's':
            size = MAX_PACKETLEN_WRITABLE_DEFAULT;
            break;
        default:
            return;
        }
    }

    if (cls.demo.recording) {
        format_demo_status(buffer, sizeof(buffer));
        Com_Printf("Already recording (%s).\n", buffer);
        return;
    }

    if (!cmd_optarg[0]) {
        Com_Printf("Missing filename argument.\n");
        Cmd_PrintHint();
        return;
    }

    if (cls.state != ca_active) {
        Com_Printf("You must be in a level to record.\n");
        return;
    }

    //
    // open the demo file
    //
    f = FS_EasyOpenFile(buffer, sizeof(buffer), mode,
                        "demos/", cmd_optarg, ".dm2");
    if (!f) {
        return;
    }

    Com_Printf("Recording client demo to %s.\n", buffer);

    cls.demo.recording = f;
    cls.demo.paused = false;

    // the first frame will be delta uncompressed
    cls.demo.last_server_frame = -1;

    SZ_Init(&cls.demo.buffer, demo_buffer, size);

    // clear dirty configstrings
    memset(cl.dcs, 0, sizeof(cl.dcs));

    // tell the server we are recording
    CL_UpdateRecordingSetting();

    //
    // write out messages to hold the startup information
    //

    if (cl_demoformat->integer > 0) {
        uint32_t magic = LittleLong(DM2X_MAGIC);
        FS_Write(&magic, sizeof(magic), cls.demo.recording);
        cls.demo.recording_type = DEM_PREDICTION_EXTENSION;
    }

    // send the serverdata
    MSG_WriteByte(svc_serverdata);
    if (cls.demo.recording_type == DEM_PREDICTION_EXTENSION) {
        MSG_WriteLong(cls.serverProtocol);
    } else {
        MSG_WriteLong(PROTOCOL_VERSION_DEFAULT);
    }
    MSG_WriteLong(0x10000 + cl.servercount);
    MSG_WriteByte(1);      // demos are always attract loops
    MSG_WriteString(cl.gamedir);
    MSG_WriteShort(cl.clientNum);
    MSG_WriteString(cl.configstrings[CS_NAME]);

    if (cls.demo.recording_type == DEM_PREDICTION_EXTENSION) {
        // send protocol specific stuff. we only really do this to store special
        // physics mods in the demo (e.g. QW mode) for accurate prediction
        switch (cls.serverProtocol) {
        case PROTOCOL_VERSION_R1Q2:
            MSG_WriteByte(0);   // not enhanced
            MSG_WriteShort(cls.protocolVersion);
            MSG_WriteByte(0);   // no advanced deltas
            MSG_WriteByte(cl.pmp.strafehack);
            break;
        case PROTOCOL_VERSION_Q2PRO:
            MSG_WriteShort(cls.protocolVersion);
            MSG_WriteByte(cl.serverstate);
            MSG_WriteByte(cl.pmp.strafehack);
            MSG_WriteByte(cl.pmp.qwmode);
            if (cls.protocolVersion >= PROTOCOL_VERSION_Q2PRO_WATERJUMP_HACK) {
                MSG_WriteByte(cl.pmp.waterhack);
            }
            break;
        default:
            break;
        }
    }

    // configstrings
    for (i = 0; i < MAX_CONFIGSTRINGS; i++) {
        s = cl.configstrings[i];
        if (!*s)
            continue;

        len = strlen(s);
        if (len > MAX_QPATH)
            len = MAX_QPATH;

        if (msg_write.cursize + len + 4 > size) {
            if (!CL_WriteDemoMessage(&msg_write))
                return;
        }

        MSG_WriteByte(svc_configstring);
        MSG_WriteShort(i);
        MSG_WriteData(s, len);
        MSG_WriteByte(0);
    }

    // baselines
    for (i = 1; i < MAX_EDICTS; i++) {
        ent = &cl.baselines[i];
        if (!ent->number)
            continue;

        if (msg_write.cursize + 64 > size) {
            if (!CL_WriteDemoMessage(&msg_write))
                return;
        }

        MSG_WriteByte(svc_spawnbaseline);
        MSG_PackEntity(&pack, ent, false);
        msgEsFlags_t flags = MSG_ES_FORCE;
        if (cls.demo.recording_type == DEM_PREDICTION_EXTENSION) {
            flags |= cl.esFlags & MSG_ES_LONGSOLID;
        }
        MSG_WriteDeltaEntity(NULL, &pack, flags);
    }

    MSG_WriteByte(svc_stufftext);
    MSG_WriteString("precache\n");

    // write it to the demo file
    CL_WriteDemoMessage(&msg_write);

    // the rest of the demo file will be individual frames
}

// resumes demo recording after pause or seek. tries to fit flushed
// configstrings and frame into single packet for seamless 'stitch'
static void resume_record(void)
{
    int i, j, index;
    size_t len;
    char *s;

    // write dirty configstrings
    for (i = 0; i < CS_BITMAP_LONGS; i++) {
        if (((uint32_t *)cl.dcs)[i] == 0)
            continue;

        index = i << 5;
        for (j = 0; j < 32; j++, index++) {
            if (!Q_IsBitSet(cl.dcs, index))
                continue;

            s = cl.configstrings[index];

            len = strlen(s);
            if (len > MAX_QPATH)
                len = MAX_QPATH;

            if (cls.demo.buffer.cursize + len + 4 > cls.demo.buffer.maxsize) {
                if (!CL_WriteDemoMessage(&cls.demo.buffer))
                    return;
                // multiple packets = not seamless
            }

            SZ_WriteByte(&cls.demo.buffer, svc_configstring);
            SZ_WriteShort(&cls.demo.buffer, index);
            SZ_Write(&cls.demo.buffer, s, len);
            SZ_WriteByte(&cls.demo.buffer, 0);
        }
    }

    // write delta uncompressed frame
    //cls.demo.last_server_frame = -1;
    CL_EmitDemoFrame();

    // FIXME: write layout if it fits? most likely it won't

    // write it to the demo file
    CL_WriteDemoMessage(&cls.demo.buffer);
}

static void CL_Suspend_f(void)
{
    if (!cls.demo.recording) {
        Com_Printf("Not recording a demo.\n");
        return;
    }

    if (!cls.demo.paused) {
        Com_Printf("Suspended demo recording.\n");
        cls.demo.paused = true;
        return;
    }

    resume_record();

    if (!cls.demo.recording)
        // write failed
        return;

    Com_Printf("Resumed demo recording.\n");

    cls.demo.paused = false;

    // clear dirty configstrings
    memset(cl.dcs, 0, sizeof(cl.dcs));
}

static int read_demo_server_message_prediction_extension(
    qhandle_t f, unsigned *cmd_acknowledged_out, byte *current_msec_out)
{
    int read;
    unsigned cmd_acknowledged;
    byte current_msec;

    read = FS_Read(&cmd_acknowledged, sizeof(cmd_acknowledged), f);
    if (read != sizeof(cmd_acknowledged)) {
        return Q_ERR_UNEXPECTED_EOF;
    }
    if (cmd_acknowledged_out) {
        *cmd_acknowledged_out = LittleLong(cmd_acknowledged);
    }

    read = FS_Read(&current_msec, sizeof(current_msec), f);
    if (read != sizeof(current_msec)) {
        return Q_ERR_UNEXPECTED_EOF;
    }
    if (current_msec_out) {
        *current_msec_out = current_msec;
    }

    return 0;
}

static int read_first_message(qhandle_t f)
{
    uint32_t    ul;
    uint16_t    us;
    size_t      msglen;
    int         read, type;

    // read magic/msglen
    read = FS_Read(&ul, 4, f);
    if (read != 4) {
        return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
    }

    // determine demo type
    if (ul == MVD_MAGIC) {
        read = FS_Read(&us, 2, f);
        if (read != 2) {
            return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
        }
        if (!us) {
            return Q_ERR_UNEXPECTED_EOF;
        }
        msglen = LittleShort(us);
        type = DEM_MVD;
    } else {
        if (ul == DM2X_MAGIC) {
            byte c;
            read = FS_Read(&c, sizeof(c), f);
            if (read != sizeof(c)) {
                return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
            }
            if (c != dem_server_message) {
                return Q_ERR_INVALID_FORMAT;
            }

            const int error = read_demo_server_message_prediction_extension(f, NULL, NULL);
            if (error) {
                return error;
            }

            cls.demo.incoming_acknowledged = 0;
            demo_outgoing_sequence = 0;
            memset(cl.history, 0, sizeof(cl.history));

            read = FS_Read(&ul, 4, f);
            if (read != 4) {
                return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
            }
            type = DEM_PREDICTION_EXTENSION;
        } else {
            type = DEM_VANILLA;
        }
        if (ul == (uint32_t)-1) {
            return Q_ERR_UNEXPECTED_EOF;
        }
        msglen = LittleLong(ul);
    }

    if (msglen < 64 || msglen > sizeof(msg_read_buffer)) {
        return Q_ERR_INVALID_FORMAT;
    }

    SZ_Init(&msg_read, msg_read_buffer, sizeof(msg_read_buffer));
    msg_read.cursize = msglen;

    // read packet data
    read = FS_Read(msg_read.data, msglen, f);
    if (read != msglen) {
        return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
    }

    return type;
}

static int read_demo_server_message(qhandle_t f)
{
    uint32_t msglen;
    int read;

    // read msglen
    read = FS_Read(&msglen, 4, f);
    if (read != 4) {
        return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
    }

    // check for EOF packet
    if (msglen == (uint32_t)-1) {
        return 0;
    }

    msglen = LittleLong(msglen);
    if (msglen > sizeof(msg_read_buffer)) {
        return Q_ERR_INVALID_FORMAT;
    }

    SZ_Init(&msg_read, msg_read_buffer, sizeof(msg_read_buffer));
    msg_read.cursize = msglen;

    // read packet data
    read = FS_Read(msg_read.data, msglen, f);
    if (read != msglen) {
        return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
    }

    return 1;
}

static int read_demo_cmd(qhandle_t f, unsigned *cmd_number_out, usercmd_t *cmd_out)
{
    int read;
    unsigned cmd_number;
    usercmd_t cmd;

    read = FS_Read(&cmd_number, sizeof(cmd_number), f);
    if (read != sizeof(cmd_number)) {
        return Q_ERR_UNEXPECTED_EOF;
    }
    if (cmd_number_out) {
        *cmd_number_out = LittleLong(cmd_number);
    }

    read = FS_Read(&cmd, sizeof(cmd), f);
    if (read != sizeof(cmd)) {
        return Q_ERR_INVALID_FORMAT;
    }
    for (int j = 0; j < 3; j++)
        cmd.angles[j] = LittleShort(cmd.angles[j]);
    cmd.forwardmove = LittleShort(cmd.forwardmove);
    cmd.sidemove    = LittleShort(cmd.sidemove);
    cmd.upmove      = LittleShort(cmd.upmove);
    if (cmd_out) {
        *cmd_out = cmd;
    }

    return 0;
}

static int read_next_cmd(qhandle_t f, usercmd_t* next_cmd_out, int* next_cmd_time_out)
{
    byte c;
    int read;
    int status;

    while (true) {
        read = FS_Read(&c, sizeof(c), f);
        if (read != sizeof(c)) {
            return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
        }

        if (c == dem_server_message) {
            byte current_msec;
            status = read_demo_server_message_prediction_extension(f, NULL, &current_msec);
            if (status) {
                return status;
            }

            status = read_demo_server_message(f);
            if (status <= 0) {
                return status;
            }
            *next_cmd_time_out = cl.servertime - current_msec;

        } else if (c == dem_usercmd) {
            status = read_demo_cmd(f, NULL, next_cmd_out);
            if (status) {
                return status;
            }
            return 0;

        } else {
            return Q_ERR_INVALID_FORMAT;
        }
    }
}

static const int status_no_server_message = 42;
static int read_next_message(qhandle_t f, demoType_t demo_type)
{
    int error;
    int read;
    byte c = dem_server_message;

    if (demo_type == DEM_PREDICTION_EXTENSION) {
        read = FS_Read(&c, sizeof(c), f);
        if (read != sizeof(c)) {
            return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
        }

        if (c == dem_server_message) {
            unsigned cmd_acknowledged;
            const int error = read_demo_server_message_prediction_extension(f, &cmd_acknowledged, NULL);
            if (error) {
                return error;
            }

            // look for the cmd number that the server acknowledged in the cmd
            // history and set the incoming_acknowledged index to it, such that
            // prediction starts correctly after that cmd
            for (int i = 1; i <= CMD_BACKUP; ++i) {
                const int new_incoming_ack = cls.demo.incoming_acknowledged + i;
                if (cl.history[new_incoming_ack & CMD_MASK].cmdNumber == cmd_acknowledged) {
                    cls.demo.incoming_acknowledged = new_incoming_ack;
                    break;
                }
            }
        }
    }

    if (c == dem_server_message) {
        return read_demo_server_message(f);

    } else if (c == dem_usercmd) {
        usercmd_t cmd;
        error = read_demo_cmd(f, &cl.cmdNumber, &cmd);
        if (error) {
            return error;
        }

        cl.cmds[cl.cmdNumber & CMD_MASK] = cmd;
        cl.history[demo_outgoing_sequence & CMD_MASK].cmdNumber = cl.cmdNumber;
        demo_outgoing_sequence += 1;

        const int msec_late = cl.time - demo_next_cmd_time;
        // peek ahead for the next cmd so we can smoothly interpolate towards it
        const uint64_t filepos = FS_Tell(cls.demo.playback);
        error = read_next_cmd(cls.demo.playback, &demo_next_cmd, &demo_next_cmd_time);
        if (error) {
            return error;
        }
        FS_Seek(cls.demo.playback, filepos);
        demo_next_cmd_time += demo_next_cmd.msec;
        cl.cmd = demo_next_cmd;
        cl.cmd.msec = msec_late;

        return status_no_server_message;
    }

    return Q_ERR_INVALID_FORMAT;
}

static void finish_demo(int ret)
{
    char *s = Cvar_VariableString("nextserver");

    if (!s[0]) {
        if (ret == 0) {
            Com_Error(ERR_DISCONNECT, "Demo finished");
        } else {
            Com_Error(ERR_DROP, "Couldn't read demo: %s", Q_ErrorString(ret));
        }
    }

    CL_Disconnect(ERR_RECONNECT);

    Cvar_Set("nextserver", "");

    Cbuf_AddText(&cmd_buffer, s);
    Cbuf_AddText(&cmd_buffer, "\n");
    Cbuf_Execute(&cmd_buffer);
}

static void update_status(void)
{
    if (cls.demo.file_size) {
        int64_t pos = FS_Tell(cls.demo.playback);

        if (pos > cls.demo.file_offset)
            cls.demo.file_percent = (pos - cls.demo.file_offset) * 100 / cls.demo.file_size;
        else
            cls.demo.file_percent = 0;
    }
}

static int parse_next_message(int wait)
{
    int ret;

    ret = read_next_message(cls.demo.playback, cls.demo.playback_type);
    if (ret < 0 || (ret == 0 && wait == 0)) {
        finish_demo(ret);
        return -1;
    }

    update_status();

    if (ret == 0) {
        cls.demo.eof = true;
        return -1;
    }

    if (ret == status_no_server_message) {
        return 0;
    }

    CL_ParseServerMessage();

    // if recording demo, write the message out
    if (cls.demo.recording && !cls.demo.paused && CL_FRAMESYNC) {
        CL_WriteDemoMessage(&cls.demo.buffer);
    }

    // if running GTV server, transmit to client
    CL_GTV_Transmit();

    // save a snapshot once the full packet is parsed
    CL_EmitDemoSnapshot();

    return 0;
}

/*
====================
CL_PlayDemo_f
====================
*/
static void CL_PlayDemo_f(void)
{
    char name[MAX_OSPATH];
    qhandle_t f;
    int type;

    if (Cmd_Argc() < 2) {
        Com_Printf("Usage: %s <filename>\n", Cmd_Argv(0));
        return;
    }

    f = FS_EasyOpenFile(name, sizeof(name), FS_MODE_READ | FS_FLAG_GZIP,
                        "demos/", Cmd_Argv(1), ".dm2");
    if (!f) {
        return;
    }

    type = read_first_message(f);
    if (type < 0) {
        Com_Printf("Couldn't read %s: %s\n", name, Q_ErrorString(type));
        FS_FCloseFile(f);
        return;
    }

    if (type == DEM_MVD) {
#if USE_MVD_CLIENT
        Cbuf_InsertText(&cmd_buffer, va("mvdplay --replace @@ \"/%s\"\n", name));
#else
        Com_Printf("MVD support was not compiled in.\n");
#endif
        FS_FCloseFile(f);
        return;
    }

    // if running a local server, kill it and reissue
    SV_Shutdown("Server was killed.\n", ERR_DISCONNECT);

    CL_Disconnect(ERR_RECONNECT);

    cls.demo.playback = f;
    cls.demo.playback_type = type;
    demo_next_cmd_time = INT_MAX;
    cls.state = ca_connected;
    Q_strlcpy(cls.servername, COM_SkipPath(name), sizeof(cls.servername));
    cls.serverAddress.type = NA_LOOPBACK;

    Con_Popup(true);
    SCR_UpdateScreen();

    // parse the first message just read
    CL_ParseServerMessage();

    // read and parse messages util `precache' command
    while (cls.state == ca_connected) {
        Cbuf_Execute(&cl_cmdbuf);
        parse_next_message(0);
    }
}

static void CL_Demo_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        FS_File_g("demos", "*.dm2;*.dm2.gz;*.mvd2;*.mvd2.gz", FS_SEARCH_SAVEPATH | FS_SEARCH_BYFILTER, ctx);
    }
}

typedef struct {
    list_t entry;
    int framenum;
    int64_t filepos;
    size_t msglen;
    byte data[1];
} demosnap_t;

/*
====================
CL_EmitDemoSnapshot

Periodically builds a fake demo packet used to reconstruct delta compression
state, configstrings and layouts at the given server frame.
====================
*/
void CL_EmitDemoSnapshot(void)
{
    demosnap_t *snap;
    int64_t pos;
    char *from, *to;
    size_t len;
    server_frame_t *lastframe, *frame;
    int i, j, lastnum;

    if (cl_demosnaps->integer <= 0)
        return;

    if (cls.demo.frames_read < cls.demo.last_snapshot + cl_demosnaps->integer * 10)
        return;

    if (!cl.frame.valid)
        return;

    if (!cls.demo.file_size)
        return;

    pos = FS_Tell(cls.demo.playback);
    if (pos < cls.demo.file_offset)
        return;

    // write all the backups, since we can't predict what frame the next
    // delta will come from
    lastframe = NULL;
    lastnum = -1;
    for (i = 0; i < UPDATE_BACKUP; i++) {
        j = cl.frame.number - (UPDATE_BACKUP - 1) + i;
        frame = &cl.frames[j & UPDATE_MASK];
        if (frame->number != j || !frame->valid ||
            cl.numEntityStates - frame->firstEntity > MAX_PARSE_ENTITIES) {
            continue;
        }

        emit_delta_frame(lastframe, frame, lastnum, j);
        lastframe = frame;
        lastnum = frame->number;
    }

    // write configstrings
    for (i = 0; i < MAX_CONFIGSTRINGS; i++) {
        from = cl.baseconfigstrings[i];
        to = cl.configstrings[i];

        if (!strcmp(from, to))
            continue;

        len = strlen(to);
        if (len > MAX_QPATH)
            len = MAX_QPATH;

        MSG_WriteByte(svc_configstring);
        MSG_WriteShort(i);
        MSG_WriteData(to, len);
        MSG_WriteByte(0);
    }

    // write layout
    MSG_WriteByte(svc_layout);
    MSG_WriteString(cl.layout);

    snap = Z_Malloc(sizeof(*snap) + msg_write.cursize - 1);
    snap->framenum = cls.demo.frames_read;
    snap->filepos = pos;
    snap->msglen = msg_write.cursize;
    memcpy(snap->data, msg_write.data, msg_write.cursize);
    List_Append(&cls.demo.snapshots, &snap->entry);

    Com_DPrintf("[%d] snaplen %"PRIz"\n", cls.demo.frames_read, msg_write.cursize);

    SZ_Clear(&msg_write);

    cls.demo.last_snapshot = cls.demo.frames_read;
}

static demosnap_t *find_snapshot(int framenum)
{
    demosnap_t *snap, *prev;

    if (LIST_EMPTY(&cls.demo.snapshots))
        return NULL;

    prev = LIST_FIRST(demosnap_t, &cls.demo.snapshots, entry);

    LIST_FOR_EACH(demosnap_t, snap, &cls.demo.snapshots, entry) {
        if (snap->framenum > framenum)
            break;
        prev = snap;
    }

    return prev;
}

/*
====================
CL_FirstDemoFrame

Called after the first valid frame is parsed from the demo.
====================
*/
void CL_FirstDemoFrame(void)
{
    int64_t len, ofs;

    Com_DPrintf("[%d] first frame\n", cl.frame.number);

    // save base configstrings
    memcpy(cl.baseconfigstrings, cl.configstrings, sizeof(cl.baseconfigstrings));

    // obtain file length and offset of the second frame
    len = FS_Length(cls.demo.playback);
    ofs = FS_Tell(cls.demo.playback);
    if (len > 0 && ofs > 0) {
        cls.demo.file_offset = ofs;
        cls.demo.file_size = len - ofs;
    }

    // begin timedemo
    if (com_timedemo->integer) {
        cls.demo.time_frames = 0;
        cls.demo.time_start = Sys_Milliseconds();
    }

    // force initial snapshot
    cls.demo.last_snapshot = INT_MIN;
}

static void CL_Seek_f(void)
{
    demosnap_t *snap;
    int i, j, ret, index, frames, dest, prev;
    char *from, *to;

    if (Cmd_Argc() < 2) {
        Com_Printf("Usage: %s [+-]<timespec>\n", Cmd_Argv(0));
        return;
    }

#if USE_MVD_CLIENT
    if (sv_running->integer == ss_broadcast) {
        Cbuf_InsertText(&cmd_buffer, va("mvdseek \"%s\" @@\n", Cmd_Argv(1)));
        return;
    }
#endif

    if (!cls.demo.playback) {
        Com_Printf("Not playing a demo.\n");
        return;
    }

    to = Cmd_Argv(1);

    if (*to == '-' || *to == '+') {
        // relative to current frame
        if (!Com_ParseTimespec(to + 1, &frames)) {
            Com_Printf("Invalid relative timespec.\n");
            return;
        }
        if (*to == '-')
            frames = -frames;
        dest = cls.demo.frames_read + frames;
    } else {
        // relative to first frame
        if (!Com_ParseTimespec(to, &dest)) {
            Com_Printf("Invalid absolute timespec.\n");
            return;
        }
        frames = dest - cls.demo.frames_read;
    }

    if (!frames)
        // already there
        return;

    if (frames > 0 && cls.demo.eof && cl_demowait->integer)
        // already at end
        return;

    // disable effects processing
    cls.demo.seeking = true;

    // clear dirty configstrings
    memset(cl.dcs, 0, sizeof(cl.dcs));

    // stop sounds
    S_StopAllSounds();

    // save previous server frame number
    prev = cl.frame.number;

    Com_DPrintf("[%d] seeking to %d\n", cls.demo.frames_read, dest);

    // seek to the previous most recent snapshot
    if (frames < 0 || cls.demo.last_snapshot > cls.demo.frames_read) {
        snap = find_snapshot(dest);

        if (snap) {
            Com_DPrintf("found snap at %d\n", snap->framenum);
            ret = FS_Seek(cls.demo.playback, snap->filepos);
            if (ret < 0) {
                Com_EPrintf("Couldn't seek demo: %s\n", Q_ErrorString(ret));
                goto done;
            }

            // clear end-of-file flag
            cls.demo.eof = false;

            // reset configstrings
            for (i = 0; i < MAX_CONFIGSTRINGS; i++) {
                from = cl.baseconfigstrings[i];
                to = cl.configstrings[i];

                if (!strcmp(from, to))
                    continue;

                Q_SetBit(cl.dcs, i);
                strcpy(to, from);
            }

            SZ_Init(&msg_read, snap->data, snap->msglen);
            msg_read.cursize = snap->msglen;

            CL_SeekDemoMessage();
            cls.demo.frames_read = snap->framenum;
            Com_DPrintf("[%d] after snap parse %d\n", cls.demo.frames_read, cl.frame.number);
        } else if (frames < 0) {
            Com_Printf("Couldn't seek backwards without snapshots!\n");
            goto done;
        }
    }

    // skip forward to destination frame
    while (cls.demo.frames_read < dest) {
        ret = read_next_message(cls.demo.playback, cls.demo.playback_type);
        if (ret == 0 && cl_demowait->integer) {
            cls.demo.eof = true;
            break;
        }
        if (ret <= 0) {
            finish_demo(ret);
            return;
        }

        CL_SeekDemoMessage();
        CL_EmitDemoSnapshot();
    }

    Com_DPrintf("[%d] after skip %d\n", cls.demo.frames_read, cl.frame.number);

    // update dirty configstrings
    for (i = 0; i < CS_BITMAP_LONGS; i++) {
        if (((uint32_t *)cl.dcs)[i] == 0)
            continue;

        index = i << 5;
        for (j = 0; j < 32; j++, index++) {
            if (Q_IsBitSet(cl.dcs, index))
                CL_UpdateConfigstring(index);
        }
    }

    // don't lerp to old
    memset(&cl.oldframe, 0, sizeof(cl.oldframe));
#if USE_FPS
    memset(&cl.oldkeyframe, 0, sizeof(cl.oldkeyframe));
#endif

    // clear old effects
    CL_ClearEffects();
    CL_ClearTEnts();

    // fix time delta
    cl.serverdelta += cl.frame.number - prev;

    // fire up destination frame
    CL_DeltaFrame();

    if (cls.demo.recording && !cls.demo.paused)
        resume_record();

    update_status();

    cl.frameflags = 0;

done:
    cls.demo.seeking = false;
}

static void parse_info_string(demoInfo_t *info, int clientNum, int index, const char *string)
{
    size_t len;
    char *p;

    if (index >= CS_PLAYERSKINS && index < CS_PLAYERSKINS + MAX_CLIENTS) {
        if (index - CS_PLAYERSKINS == clientNum) {
            Q_strlcpy(info->pov, string, sizeof(info->pov));
            p = strchr(info->pov, '\\');
            if (p) {
                *p = 0;
            }
        }
    } else if (index == CS_MODELS + 1) {
        len = strlen(string);
        if (len > 9) {
            memcpy(info->map, string + 5, len - 9);   // skip "maps/"
            info->map[len - 9] = 0; // cut off ".bsp"
        }
    }
}

/*
====================
CL_GetDemoInfo
====================
*/
demoInfo_t *CL_GetDemoInfo(const char *path, demoInfo_t *info)
{
    qhandle_t f;
    int c, index;
    char string[MAX_QPATH];
    int clientNum, type;

    FS_FOpenFile(path, &f, FS_MODE_READ | FS_FLAG_GZIP);
    if (!f) {
        return NULL;
    }

    type = read_first_message(f);
    if (type < 0) {
        goto fail;
    }

    if (type == DEM_VANILLA || type == DEM_PREDICTION_EXTENSION) {
        if (MSG_ReadByte() != svc_serverdata) {
            goto fail;
        }
        const int serverProtocol = MSG_ReadLong();
        if (serverProtocol != PROTOCOL_VERSION_DEFAULT && type == DEM_VANILLA) {
            goto fail;
        }
        MSG_ReadLong();
        MSG_ReadByte();
        MSG_ReadString(NULL, 0);
        clientNum = MSG_ReadShort();
        MSG_ReadString(NULL, 0);

        if (serverProtocol == PROTOCOL_VERSION_R1Q2) {
            MSG_ReadByte();
            MSG_ReadShort();
            MSG_ReadByte();
            MSG_ReadByte();
        } else if (serverProtocol == PROTOCOL_VERSION_Q2PRO) {
            const int protocolVersion = MSG_ReadShort();
            MSG_ReadByte();
            MSG_ReadByte();
            MSG_ReadByte();
            if (protocolVersion >= PROTOCOL_VERSION_Q2PRO_WATERJUMP_HACK) {
                MSG_ReadByte();
            }
        }

        while (1) {
            c = MSG_ReadByte();
            if (c == -1) {
                if (read_next_message(f, type) <= 0) {
                    break;
                }
                continue; // parse new message
            }
            if (c != svc_configstring) {
                break;
            }
            index = MSG_ReadShort();
            if (index < 0 || index >= MAX_CONFIGSTRINGS) {
                goto fail;
            }
            MSG_ReadString(string, sizeof(string));
            parse_info_string(info, clientNum, index, string);
        }

        info->mvd = false;
    } else {
        if ((MSG_ReadByte() & SVCMD_MASK) != mvd_serverdata) {
            goto fail;
        }
        if (MSG_ReadLong() != PROTOCOL_VERSION_MVD) {
            goto fail;
        }
        MSG_ReadShort();
        MSG_ReadLong();
        MSG_ReadString(NULL, 0);
        clientNum = MSG_ReadShort();

        while (1) {
            index = MSG_ReadShort();
            if (index == MAX_CONFIGSTRINGS) {
                break;
            }
            if (index < 0 || index >= MAX_CONFIGSTRINGS) {
                goto fail;
            }
            MSG_ReadString(string, sizeof(string));
            parse_info_string(info, clientNum, index, string);
        }

        info->mvd = true;
    }

    FS_FCloseFile(f);
    return info;

fail:
    FS_FCloseFile(f);
    return NULL;

}

// =========================================================================

void CL_CleanupDemos(void)
{
    demosnap_t *snap, *next;
    size_t total;

    if (cls.demo.recording) {
        CL_Stop_f();
    }

    if (cls.demo.playback) {
        FS_FCloseFile(cls.demo.playback);

        if (com_timedemo->integer && cls.demo.time_frames) {
            unsigned msec = Sys_Milliseconds();

            if (msec > cls.demo.time_start) {
                float sec = (msec - cls.demo.time_start) * 0.001f;
                float fps = cls.demo.time_frames / sec;

                Com_Printf("%u frames, %3.1f seconds: %3.1f fps\n",
                           cls.demo.time_frames, sec, fps);
            }
        }
    }

    total = 0;
    LIST_FOR_EACH_SAFE(demosnap_t, snap, next, &cls.demo.snapshots, entry) {
        total += snap->msglen;
        Z_Free(snap);
    }

    if (total)
        Com_DPrintf("Freed %"PRIz" bytes of snaps\n", total);

    memset(&cls.demo, 0, sizeof(cls.demo));

    List_Init(&cls.demo.snapshots);
}

/*
====================
CL_DemoFrame
====================
*/
void CL_DemoFrame(int msec)
{
    if (cls.state < ca_connected) {
        return;
    }

    if (cls.state != ca_active) {
        parse_next_message(0);
        return;
    }

    if (com_timedemo->integer) {
        parse_next_message(0);
        cl.time = cl.servertime;
        cls.demo.time_frames++;
        return;
    }

    // wait at the end of demo
    if (cls.demo.eof) {
        if (!cl_demowait->integer)
            finish_demo(0);
        return;
    }

    // For the prediction extension: count up the next cmd to smoothly use
    // more and more of it in prediction
    cl.cmd.msec += msec;

    // cl.time has already been advanced for this client frame
    // read the next frame to start lerp cycle again
    while (cl.servertime < cl.time || demo_next_cmd_time <= cl.time) {
        if (parse_next_message(cl_demowait->integer))
            break;
        if (cls.state != ca_active)
            break;
    }

    // For the prediction extension: The angle of the current cl.cmd is applied
    // immediately in the prediction code. So to make it smooth, we interpolate
    // between the previous cmd already stored in cl.cmds and the upcoming one
    // in demo_next_cmd.
    for (int i = 0; i < 3; ++i) {
        const double frac = (1.0 * cl.cmd.msec) / demo_next_cmd.msec;
        double old = SHORT2ANGLE(cl.cmds[cl.cmdNumber & CMD_MASK].angles[i]);
        double new = SHORT2ANGLE(demo_next_cmd.angles[i]);
        cl.cmd.angles[i] = ANGLE2SHORT(LerpAngle(old, new, frac));
    }
}

static const cmdreg_t c_demo[] = {
    { "demo", CL_PlayDemo_f, CL_Demo_c },
    { "record", CL_Record_f, CL_Demo_c },
    { "stop", CL_Stop_f },
    { "suspend", CL_Suspend_f },
    { "seek", CL_Seek_f },

    { NULL }
};

/*
====================
CL_InitDemos
====================
*/
void CL_InitDemos(void)
{
    cl_demosnaps = Cvar_Get("cl_demosnaps", "10", 0);
    cl_demomsglen = Cvar_Get("cl_demomsglen", va("%d", MAX_PACKETLEN_WRITABLE_DEFAULT), 0);
    cl_demowait = Cvar_Get("cl_demowait", "0", 0);
    cl_demoformat = Cvar_Get("cl_demoformat", "2", 0);

    Cmd_Register(c_demo);
    List_Init(&cls.demo.snapshots);
}

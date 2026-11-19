/* Included by cd_shared.c, so the join can start the map's named cdtrack
 * without queueing a command behind a blocking world load. */
void TouchMusic_PlayTrack(const char *track);
void TouchMusic_PlayTrack(const char *track)
{
	static char said[MAX_QPATH];
	if (music_playlist_index.integer < 0)
	{
		CDAudio_Play_byName(track, true, true, 0);
		if (CDAudio_IsPlaying() && strcmp(said, track))
		{
			dp_strlcpy(said, track, sizeof(said));
			Con_Printf("MUSIC: map track %s starts during the join\n", track);
		}
	}
}

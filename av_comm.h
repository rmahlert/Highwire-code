/*
 * @(#) Gemini\vaproto.h
 * @(#) Stefan Eissing, December 11, 1994
 *
 *
 * Description: Definition of messages for the Venus <-> Accessory
 * Protocol
 *
 * Dec 07: AV_PATH_UPDATE, AV_WHAT_IZIT, AV_DRAG_ON_WINDOW added.
 * Oct 21, 94: AV_STARTED
 * Oct 31, 94: AV_XWIND and VA_FONTCHANGED introduced.
 * See also the new bit assignment in PROTOSTATUS
 * Nov 12, 94 New bit in the Accessory's PROTOSTATUS for "quoting"
 * of filenames
 */

#ifndef __vaproto__
#define __vaproto__

/* Message numbers for the xAcc protocol by Konrad Hinsen
 * Venus returns "VENUS.APP" for ACC_ID and Gemini returns "GEMINI.APP".
 * Gemini supports xAcc level 0.
 */
#define ACC_ID		0x400
#define ACC_OPEN	0x401
#define ACC_CLOSE	0x402
#define ACC_ACC		0x403

/* Message numbers for communication between Venus and
 * various Accessories.
 * If paths or filenames appear in messages, the absolute path
 * must always be specified (i.e., with drive letter)
 * and drive letters must be uppercase. Paths
 * should always end with a backslash!
 * New since November 12, 1994, names can optionally be enclosed
 * in single quotes. See AV_PROTOCOL.
 *
 * Messages from Venus start with VA (Venus -> Accessory).
 * Messages to Venus start with AV (Accessory -> Venus).
 *
 * With AV_PROTOCOL, each Acc can ask which messages
 * are understood (This is different for VENUS and GEMINI!
 * It would be nice if other programs would also react to
 * this protocol. At least AV_SENDKEY is
 * certainly easy to implement and is the only method by which
 * keyboard presses can be simulated via messages.
 *
 * Under normal TOS, an Accessory asks application 0 with
 * AV_PROTOCOL if it understands anything about it, when it has received
 * an AC_CLOSE message from the AES.
 * Under MultiTOS, however, the program no longer needs to have ID 0,
 * and AC_CLOSE messages (almost) no longer occur.
 * Not only Accessories, but also normal programs might want
 * to communicate with Gemini. What to do?
 * If there can be more than one main application, the program/accessory
 * should try to contact GEMINI.
 * The ID can be determined with appl_find. If this fails,
 * one can still search for AVSERVER or the content of the (AES-)Environment
 * variable AVSERVER. Especially the latter option allows
 * easy configuration "from the outside". The new versions of
 * the library VAFUNC by Stephan Gerle (available in well-stocked
 * mailboxes) proceed almost exactly the same way.
 */

/* AV_PROTOCOL: With this message number, one should ask
 * other applications and GEMINI if
 * and which messages they understand.
 */

#define AV_PROTOCOL		0x4700
/*
 * Word 6+7: Pointer to the Accessory name, as it must be used
 * with appl_find; i.e. 8 characters long
 * null-terminated (char name[9]).
 * The bits in words 3, 4 and 5 have the following meaning:
 * Word 3:
 * Bit 0:		(VA_SETSTATUS)
 * Bit 1:		(VA_START)
 * Bit 2:       (AV_STARTED)
 * Bit 3:       (VA_FONTCHANGED)
 * Bit 4:       (Understands and uses quoting of filenames)
 *
 * All other bits are reserved for extensions and should
 * therefore be pre-set to 0. This also applies, of course, to the
 * bits in words 4 and 5.
 * (More on quoting see below)
 */

/* Macros for testing the protocol status for quoting
 */
#define VA_ACC_QUOTING(a)		((a) & 0x10)
#define VA_SERVER_QUOTING(a)	((a) & 0x4000)

/* VA_PROTOSTATUS: The sender of AV_PROTOCOL is informed that
 * the recipient knows something about this protocol. Words 3-7 of the
 * message buffer contain the information about which messages
 * are understood. Set bits mean that a message
 * (message group) is understood.
 */
#define VA_PROTOSTATUS		0x4701
/*
 * Word 6+7: Pointer to the program name, as it must be used
 * with appl_find; i.e. 8 characters long
 * null-terminated (char name[9]).
 * The bits in words 3, 4 and 5 have the following meaning:
 * Word 3:
 * Bit 0		(AV_SENDKEY)
 * Bit 1		(AV_ASKFILEFONT)
 * Bit 2		(AV_ASKCONFONT, AV_OPENCONSOLE)
 * Bit 3		(AV_ASKOBJECT)
 * Bit 4		(AV_OPENWIND)
 * Bit 5		(AV_STARTPROG)
 * Bit 6		(AV_ACCWINDOPEN, AV_ACCWINDCLOSED)
 * Bit 7		(AV_STATUS, AV_GETSTATUS)
 * Bit 8		(AV_COPY_DRAGGED)
 * Bit 9        (AV_PATH_UPDATE, AV_WHAT_IZIT, AV_DRAG_ON_WINDOW)
 * Bit 10		(AV_EXIT)
 * Bit 11       (AV_XWIND)
 * Bit 12       (VA_FONTCHANGED)
 * Bit 13		(AV_STARTED)
 * Bit 14       (Understands and uses quoting of filenames)
 *
 * All other bits are reserved for extensions and should
 * therefore be pre-set to 0. This also applies, of course, to the
 * bits in words 4 and 5.
 *
 * AV_SENDKEY can certainly be easily integrated into any program.
 * For AV_OPENWIND, a main program could also launch its "normal"
 * routine for opening a document and use the
 * passed path. This, together with the use
 * of TreeView, is certainly a simple way to load files from other
 * folders or drives.
 *
 * Regarding Bit 14 (in the Server, e.g. Gemini), or Bit 4 in the Client
 * (Accessory):
 * "Quoting" in the VA-protocol means that filenames can optionally
 * be enclosed in single quotes 'name'.
 * However, this is only allowed if both parties (Server and Client)
 * agree on it (read: both can understand it).
 * So how does this work? Suppose Gemini sends a filename containing
 * a space to an Accessory. An Acc that cannot quote will recognize
 * two filenames instead of one, as spaces normally separate filenames.
 * If the Acc understands quoting (Bit 4 in its protocol status),
 * Gemini will enclose the filename in '' and the Acc recognizes
 * that the space belongs to the filename.
 * The same, of course, applies to filenames sent by an Acc
 * to a Server. The question remains how to transmit single quotes.
 * Well, such characters that belong to the filename are simply
 * doubled. From Julian's Profibuch, 'Julian''s Profibuch' is transmitted.
 * Simple, isn't it? Well, the idea is not mine, but is used
 * in exactly the same way in Atari's Drag&Drop protocol.
 */

/* AV_GETSTATUS: An Accessory asks Venus for its current
 * status, which it previously gave to Venus with AV_STATUS.
 */
#define AV_GETSTATUS		0x4703

/* AV_STATUS: An Accessory can inform Venus of its status,
 * which is then saved by Venus in the INF file and can be
 * retrieved again with AV_GETSTATUS.
 * Before that, it MUST register itself with AV_PROTOCOL!
 * Word 3+4: Pointer to a string, which must not contain control characters
 * and must not be longer than 256 characters.
 * This pointer can, however, be NULL.
 */
#define AV_STATUS			0x4704

/* VA_SETSTATUS: Venus communicates the saved status to the Accessory
 * upon request via AV_GETSTATUS. This can then be set by an Accessory.
 * Word 3+4: Pointer to a string that contains no control characters.
 * This pointer can, however, be NULL; in that case,
 * no status was saved.
 */
#define VA_SETSTATUS		0x4705

/* AV_SENDKEY: An Acc sends a keyboard event to VENUS/GEMINI that
 * it itself might not understand.
 * Word 3 = Keyboard status             ev_mmokstate
 * Word 4 = Scancode of the pressed key  ev_mkreturn
 */
#define	AV_SENDKEY			0x4710

/* VA_START: Accessory is activated. Word 3 + 4 contain a
 * pointer to a command line, which can also be NULL.
 * The command line contains paths or filenames.
 */
#define VA_START			0x4711

/* AV_ASKFILEFONT: Query for the set character set
 * for filenames.
 */
#define AV_ASKFILEFONT		0x4712

/* VA_FILEFONT: Returns the currently set character set.
 * Word 3 = File font number (font id)
 * Word 4 = File font size (in points)
 */
#define VA_FILEFONT			0x4713

/* (Gemini only) AV_ASKCONFONT: Query for the set
 * character set for the Console window.
 */
#define AV_ASKCONFONT		0x4714

/* VA_CONFONT: Returns the currently set character set.
 * Word 3 = Console font number (font id)
 * Word 4 = Console font size (in points)
 */
#define VA_CONFONT			0x4715

/* AV_ASKOBJECT: Queries for the currently selected object.
 * The name of the currently selected object is returned.
 * If no object is selected, the string is empty.
 * If multiple objects are selected, their names are separated by spaces.
 */
#define AV_ASKOBJECT	0x4716

/* VA_OBJECT: Returns names of the currently selected objects.
 * Structure as in VA_START
 */
#define VA_OBJECT		0x4717

/* (Gemini only)AV_OPENCONSOLE: Venus should open the Console window.
 * If it is already open, it is brought to the front. This action
 * is especially useful if an Accessory wants to start a TOS program
 * using the system() function (Warning: A GEM program must
 * never be started by an Accessory via system()! (see also AV_STARTPROG)
 * Also, this message should only be used at the explicit request of the
 * user, as it can otherwise only confuse them.
 *
 * ATTENTION: This message is only available in Gemini.app.
 */
#define AV_OPENCONSOLE	0x4718

/* VA_CONSOLEOPEN: Returns whether the Console window has been
 * brought to the front. Word 3 == 0 (no) != 0 (yes)
 */
#define VA_CONSOLEOPEN	0x4719

/* AV_OPENWIND: Venus should open a file window.
 * This should also only happen if the cause is
 * apparent to the user.
 * Word 3+4 (Pointer) Path for the window (see above).
 * Word 5+6 (Pointer) Wildcard for files to be displayed.
 */
#define AV_OPENWIND		0x4720

/* VA_WINDOPEN: Indicates whether the window could be opened.
 * see VA_CONSOLEOPEN
 */
#define VA_WINDOPEN		0x4721


/* Word 7 in AV_STARTPROG and Words 4 and 7 in VA_PROGSTART are
 * new since March 29, 1992.
 */

/* AV_STARTPROG: Venus should start a program. Here,
 * the registered applications of Venus are
 * considered. So you can also specify a file for
 * which Venus then searches for a program.
 * Word 3+4 (Pointer) Program name with complete path
 * Word 5+6 (Pointer) Command line (can be NULL)
 * Word 7    Arbitrary 16-bit word that is
 * returned in VA_PROGSTART.
 */
#define AV_STARTPROG	0x4722

/* VA_PROGSTART: Indicates whether Venus starts the program.
 * Word 3 == 0: not started, != 0 started
 * Generally, the Acc will immediately receive a message in case of an error.
 * However, if the program is started, the Acc will only receive
 * this message after the program has started, as the routine
 * that performs the Pexec can no longer know that an Acc still needs
 * to receive a message quickly. For a GEM program, success can also
 * be recognized by AC_CLOSE.
 * Also, error detection is not optimal. The return value does not
 * indicate that the program ran without errors.
 *
 * Word 4    Return code of the started program (if available)
 * Word 7    16-bit word from AV_STARTPROG
 */
#define VA_PROGSTART	0x4723

/* AV_ACCWINDOPEN: With this message, an Acc can inform Venus that
 * it has opened a window.
 * Word 3 AES-Handle of the opened window
 */
#define AV_ACCWINDOPEN	0x4724

/* VA_DRAGACCWIND: Venus informs the Acc that objects have been
 * dragged onto one of its windows registered with AV_ACCWINDOPEN.
 * Word 3    AES-Handle of the window
 * Word 4    X-position of the mouse
 * Word 5    Y-position of the mouse
 * Word 6+7 Pointer to a string containing the names of the objects.
 */
#define VA_DRAGACCWIND	0x4725

/* AV_ACCWINDCLOSED: Acc informs Venus that its window has been closed.
 * The Acc only needs to do this if it closes the window itself.
 * If it receives an AC_CLOSE message from the AES, Venus already
 * knows that all windows are gone.
 * Word 3    AES-Handle of the window
 */
#define AV_ACCWINDCLOSED	0x4726


/* New since April 11, 1991!!!
 */

/* AV_COPY_DRAGGED: Accessory informs Venus that the objects
 * dragged onto its window are to be copied.
 * This can be desired, for example, after dragging objects to the
 * TreeView window. This message is only intended as a response
 * to VA_DRAGACCWIND.
 * Word 3	Keyboard status (Alternate, Control, Shift)
 * Word 4+5	Pointer to a string containing the name of the target object.
 * This *must* be a path!
 */
#define AV_COPY_DRAGGED		0x4728

/* VA_COPY_COMPLETE: Reply to AV_COPY.
 * Word 3	Status of the copy. (!= 0 means that something was
 * actually copied or moved. The Acc can possibly use this
 * to rebuild its window.)
 */
#define VA_COPY_COMPLETE	0x4729


/* AV_PATH_UPDATE: Program/Accessory informs Gemini that the content
 * of a directory has changed. Gemini then redraws this directory
 * (if a window of it is open). This
 * also affects subdirectories; an update of "C:\" ensures
 * that everything related to drive C:\ is re-read.
 *
 * Word 3+4 Pointer to the absolute path
 */
#define AV_PATH_UPDATE		0x4730


/* AV_WHAT_IZIT: Program/Accessory asks Gemini what is at
 * position X/Y on the screen. The coordinates are
 * normal pixel coordinates with the origin in the top left
 * corner of the screen. The answer is VA_THAT_IZIT.
 * Word 3    X-coordinate
 * Word 4    Y-coordinate
 */
#define AV_WHAT_IZIT		0x4732

/* VA_THAT_IZIT:
 * Word 3    ID of the responsible application
 * Word 4    Type of the object
 * Word 5+6 Pointer to the name of the object (or NULL if not
 * available)
 *
 * Type is as follows: (all others reserved for extensions.)
 */
#define	VA_OB_UNKNOWN	0
#define VA_OB_TRASHCAN  1
#define VA_OB_SHREDDER  2
#define VA_OB_CLIPBOARD 3
#define VA_OB_FILE      4
#define VA_OB_FOLDER	5
#define VA_OB_DRIVE		6
#define VA_OB_WINDOW    7

#define VA_THAT_IZIT		0x4733

/* AV_DRAG_ON_WINDOW: Program/Accessory informs Gemini that
 * objects have been dragged onto one of its windows queried
 * via AV_WHATIZIT. The names are names of files, folders,
 * or drives, each separated by a space. Names
 * of folders or drives end with a backslash.
 *
 * Word 3    X-position where the mouse was dragged
 * Word 4    Y-position where the mouse was dragged
 * Word 5    Keyboard status (Shift, Control, Alternate)
 * Word 6+7 Pointer to a string containing the names of the objects.
 *
 * (now implemented, note that the message's
 * layout has slightly changed. The window handle is gone and
 * the current keyboard status has been added instead.)
 *
 */
#define AV_DRAG_ON_WINDOW	0x4734

/* VA_DRAG_COMPLETE: The action initiated via AV_DRAG_ON_WINDOW
 * (copying, moving, deleting or dropping onto the background)
 * is complete. On success, a value similar to AV_COPY_COMPLETE
 * is returned.
 * Word 3	Status of the action. (!= 0 means that something was
 * actually copied or moved. The Acc can possibly use this
 * to rebuild its window.)
 */
#define VA_DRAG_COMPLETE	0x4735

/* AV_EXIT: A program/Accessory informs Gemini that it no
 * longer participates in the protocol (normally, because it has
 * terminated).
 * Word 3    AES-ID of the program/Accessory
 */
#define AV_EXIT				0x4736

/* AV_STARTED: A program/Accessory informs Gemini that it
 * understood the VA_START message and the memory of the
 * string attached to the message can be freed. To identify
 * which VA_START message it is, the values from VA_START are returned.
 * Word 3+4: exactly the same content as the VA_START message.
 */
#define AV_STARTED      0x4738

/* VA_FONTCHANGED: One of the fonts set in Gemini has
 * changed. Sent to all applications that have already
 * queried Gemini's font.
 *
 * Word 3 = File font number    (font id)
 * Word 4 = File font size      (in points)
 * Word 5 = Console font number (font id)
 * Word 6 = Console font size   (in points)
 */
#define VA_FONTCHANGED		0x4739

/* AV_XWIND: Venus should open a file window (eXtended).
 * This should also only happen if the cause is
 * apparent to the user.
 * Word 3+4 (Pointer) Path for the window (see above).
 * Word 5+6 (Pointer) Wildcard as filter for display
 * Word 7    Bitmask  0x0001 - bring existing window to top if present
 * 0x0002 - Wildcard should only select
 * - set all other bits to 0!
 */
#define AV_XWIND		0x4740

/* VA_XOPEN: Indicates whether the window could be opened.
 * (Word 3 == 0 (no) != 0 (yes))
 */
#define VA_XOPEN		0x4741

#endif

#define PDF_AV_OPEN_FILE		0x5000									/* Opens new file										*/
#define PDF_AV_CLOSE_FILE		0x5001									/* Closes an already opened file		*/
#define PDF_AV_PRINT_FILE		0x5002									/* Print an file										*/
#define PDF_AV_FIND_WORD		0x5003									/* Looks for a word in a document	*/
#define PDF_AV_SHOW_INFO		0x5004									/* Shows info about a document			*/
#define PDF_AV_GET_INFO			0x5005									/* Gives back info about a PDF			*/

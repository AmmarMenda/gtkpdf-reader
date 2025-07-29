To compile this application you need:<br>
- libmupdf
- gtk
- gcc<br>
<br>
Use this command to compile this application:<br>
gcc -o guf main.c $(pkg-config --cflags --libs gtk4 gio-2.0  mupdf)<br>
<br>
This is how it looks<br>
<br>
<img width="751" height="674" alt="2025-07-14-59-1753781349-scrot" src="https://github.com/user-attachments/assets/47132304-c60e-4ba6-afc5-ac4f6d0af5b4" />

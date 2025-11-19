To compile this application you need:<br>
- libmupdf
- gtk
- gcc<br>
<br>
Use this command to compile this application:<br>
gcc -o guf  main.c $(pkg-config --cflags --libs gtk4) -lmupdf -lm
<br>
This is how it looks<br>
<br>
<img width="1366" height="768" alt="2025-11-23-54-1763576660-scrot" src="https://github.com/user-attachments/assets/361beb43-1cf4-43c9-b0b1-764441f10f2b" />

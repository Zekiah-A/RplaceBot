# Run instructions:
# Install packages with "python3 -m pip install prompt_toolkit"
# Run program with "python3 edit-db.py"
import sqlite3
from prompt_toolkit import PromptSession
from prompt_toolkit.history import FileHistory

def run_query(cursor, query):
    try:
        cursor.execute(query)
        rows = cursor.fetchall()
        for row in rows:
            print(row)
    except sqlite3.Error as error:
        print("Error: Could not execute query:", error)

if __name__ == "__main__":
    db_path = input("Enter database path (default 'rplace_bot.db'): ")
    if not db_path:
        db_path = "rplace_bot.db"

    print("\x1b[32mInfo: Type 'commit' to commit database changes. Type 'exit' to quit.")
    history = FileHistory("edit_db_history.txt")
    session = PromptSession(history = history)
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()

    while True:
        try:
            query = session.prompt("> ")
            if query == "commit":
                conn.commit()
            elif query == "exit":
                raise EOFError
            else:
                run_query(cursor, query)
        except KeyboardInterrupt:
            print("KeyboardInterrupt: Press Ctrl-D or type 'exit' to quit")
        except EOFError:
            print("Exiting...")
            if conn:
                conn.close()
            break
from concurrent import interpreters

# Create a subinterpreter
interp = interpreters.create()

my_list = [1, 2, 3]

# Subinterpreter has NO access to main's objects
interp.exec("""
try:
    print(my_list)
except NameError as e:
    print(f"NameError: {e}")
""")

# Share by COPYING the data
interp.exec(f"""
copied_list = {my_list}  # A copy, not a reference
print(f"Received a copy: {{copied_list}}")
""")

interp.close()
# Define the executable name
TARGET = cdocker

# Define the source files
SRCS = main.c 

# Define the object files (derived from source files)
OBJS = $(SRCS:.c=.o)

# Define the C compiler
CC = gcc

# Define compilation flags (e.g., -Wall for warnings, -g for debugging)
CFLAGS = -Wall -g

# Define linking flags (e.g., -lm for math library)
LDFLAGS =

# Default target: builds the executable
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $(TARGET)

# Rule to compile .c files into .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean target: removes generated files
clean:
	rm -f $(TARGET) $(OBJS)
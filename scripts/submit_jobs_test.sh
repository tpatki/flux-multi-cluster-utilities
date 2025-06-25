echo -e "\nSubmitting 10 test jobs..."
for i in {1..10}; do
    flux submit --quiet -N1 -n1 sleep 30
done
